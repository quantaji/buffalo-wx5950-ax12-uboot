/*
 * Buffalo WXR-5950AX12 recovery web service.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <linux/ctype.h>
#include <malloc.h>
#include <net.h>
#include <net/tcp.h>

#include "wxr5950ax12_boot.h"

#define WXR_WEB_PORT			80
#define WXR_HTTP_HEADER_MAX		4096
#define WXR_HTTP_RESPONSE_HEADER_MAX	512
#define WXR_HTTP_RESPONSE_TEXT_MAX	256
#define WXR_DHCP_HOSTNAME		"buffalo-wxr-5950ax12-uboot"

enum wxr_http_phase {
	WXR_HTTP_HEADERS,
	WXR_HTTP_BODY,
	WXR_HTTP_EXECUTE,
	WXR_HTTP_RESPONSE,
};

enum wxr_http_operation {
	WXR_HTTP_NONE,
	WXR_HTTP_PAGE,
	WXR_HTTP_BOOT_RECOVERY,
	WXR_HTTP_WRITE_NAND_RECOVERY,
	WXR_HTTP_WRITE_NAND_PRODUCTION,
	WXR_HTTP_RESET_NAND_CONFIGURATION,
	WXR_HTTP_WRITE_USB_RECOVERY,
	WXR_HTTP_WRITE_USB_PRODUCTION,
	WXR_HTTP_RESET_USB_CONFIGURATION,
	WXR_HTTP_REBOOT,
};

struct wxr_saved_env {
	const char *name;
	char *value;
	int existed;
};

struct wxr_saved_network {
	struct in_addr ip;
	struct in_addr netmask;
	struct in_addr gateway;
	struct in_addr dns_server;
#ifdef CONFIG_BOOTP_DNS2
	struct in_addr dns_server2;
#endif
	struct in_addr server_ip;
	u8 server_ethaddr[6];
	char boot_file_name[sizeof(net_boot_file_name)];
	char hostname[sizeof(net_hostname)];
};

struct wxr_http_request {
	struct tcp_stream *tcp;
	enum wxr_http_phase phase;
	enum wxr_http_operation operation;
	char header[WXR_HTTP_HEADER_MAX + 1];
	size_t header_received;
	size_t header_length;
	size_t content_length;
	char response_header[WXR_HTTP_RESPONSE_HEADER_MAX];
	char response_text[WXR_HTTP_RESPONSE_TEXT_MAX];
	const void *response_body;
	size_t response_header_length;
	size_t response_body_length;
	size_t response_total;
};

extern const unsigned char wxr_web_page_begin[];
extern const unsigned char wxr_web_page_end[];

static struct wxr_http_request wxr_http;
static cmd_tbl_t *wxr_web_cmdtp;

static int wxr_http_rx(struct tcp_stream *tcp, u32 rx_offs, void *buf,
			int len);
static int wxr_http_tx(struct tcp_stream *tcp, u32 tx_offs, void *buf,
			int maxlen);
static void wxr_http_on_rcv_nxt_update(struct tcp_stream *tcp, u32 rx_bytes);
static void wxr_http_on_snd_una_update(struct tcp_stream *tcp, u32 tx_bytes);
static void wxr_http_on_closed(struct tcp_stream *tcp);

static void wxr_http_reset_request(void)
{
	memset(&wxr_http, 0, sizeof(wxr_http));
	wxr_http.phase = WXR_HTTP_HEADERS;
}

static int wxr_http_parse_content_length(const char *value, size_t *length)
{
	size_t parsed = 0;

	while (*value == ' ' || *value == '\t')
		value++;
	if (!isdigit(*value))
		return -EINVAL;

	while (isdigit(*value)) {
		unsigned int digit = *value++ - '0';

		if (parsed > (WXR_INPUT_MAX_SIZE - digit) / 10)
			return -EFBIG;
		parsed = parsed * 10 + digit;
	}
	while (*value == ' ' || *value == '\t')
		value++;
	if (*value != '\0')
		return -EINVAL;

	*length = parsed;
	return 0;
}

static void wxr_http_set_response(int status, const char *text,
				  const void *body, size_t body_length,
				  int gzip)
{
	const char *reason;
	const char *type;

	switch (status) {
	case 200:
		reason = "OK";
		break;
	case 400:
		reason = "Bad Request";
		break;
	case 404:
		reason = "Not Found";
		break;
	case 405:
		reason = "Method Not Allowed";
		break;
	case 409:
		reason = "Conflict";
		break;
	case 411:
		reason = "Length Required";
		break;
	case 413:
		reason = "Payload Too Large";
		break;
	case 415:
		reason = "Unsupported Media Type";
		break;
	case 422:
		reason = "Unprocessable Content";
		break;
	case 431:
		reason = "Request Header Fields Too Large";
		break;
	case 507:
		reason = "Insufficient Storage";
		break;
	default:
		status = 500;
		reason = "Internal Server Error";
		break;
	}

	if (!body) {
		snprintf(wxr_http.response_text, sizeof(wxr_http.response_text),
			 "%s\n", text);
		body = wxr_http.response_text;
		body_length = strlen(wxr_http.response_text);
	}
	type = gzip ? "text/html; charset=utf-8" : "text/plain; charset=utf-8";
	wxr_http.response_header_length = snprintf(
		wxr_http.response_header, sizeof(wxr_http.response_header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %lu\r\n"
		"Connection: close\r\n%s\r\n",
		status, reason, type, (ulong)body_length,
		gzip ? "Content-Encoding: gzip\r\n" : "");
	wxr_http.response_body = body;
	wxr_http.response_body_length = body_length;
	wxr_http.response_total = wxr_http.response_header_length + body_length;
	wxr_http.phase = WXR_HTTP_RESPONSE;
}

static int wxr_http_parse_request(void)
{
	char *line;
	char *next;
	char *path;
	char *version;
	int content_length_count = 0;
	int content_type_ok = 0;
	int transfer_encoding = 0;
	int ret;

	line = wxr_http.header;
	next = strstr(line, "\r\n");
	if (!next)
		return -EINVAL;
	*next = '\0';

	if (!strncmp(line, "GET ", 4)) {
		path = line + 4;
		wxr_http.operation = WXR_HTTP_PAGE;
	} else if (!strncmp(line, "POST ", 5)) {
		path = line + 5;
	} else {
		wxr_http_set_response(405, "Only GET and POST are supported",
				      NULL, 0, 0);
		return -EINVAL;
	}

	version = strchr(path, ' ');
	if (!version) {
		wxr_http_set_response(400, "Malformed request line", NULL, 0, 0);
		return -EINVAL;
	}
	*version++ = '\0';
	if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) {
		wxr_http_set_response(400, "Unsupported HTTP version", NULL, 0, 0);
		return -EINVAL;
	}

	if (wxr_http.operation == WXR_HTTP_PAGE) {
		if (strcmp(path, "/")) {
			wxr_http_set_response(404, "Resource not found", NULL, 0, 0);
			return -EINVAL;
		}
	} else if (!strcmp(path, "/api/recovery/boot")) {
		wxr_http.operation = WXR_HTTP_BOOT_RECOVERY;
	} else if (!strcmp(path, "/api/nand/recovery")) {
		wxr_http.operation = WXR_HTTP_WRITE_NAND_RECOVERY;
	} else if (!strcmp(path, "/api/nand/production")) {
		wxr_http.operation = WXR_HTTP_WRITE_NAND_PRODUCTION;
	} else if (!strcmp(path, "/api/nand/reset")) {
		wxr_http.operation = WXR_HTTP_RESET_NAND_CONFIGURATION;
	} else if (!strcmp(path, "/api/usb/recovery")) {
		wxr_http.operation = WXR_HTTP_WRITE_USB_RECOVERY;
	} else if (!strcmp(path, "/api/usb/production")) {
		wxr_http.operation = WXR_HTTP_WRITE_USB_PRODUCTION;
	} else if (!strcmp(path, "/api/usb/reset")) {
		wxr_http.operation = WXR_HTTP_RESET_USB_CONFIGURATION;
	} else if (!strcmp(path, "/api/reboot")) {
		wxr_http.operation = WXR_HTTP_REBOOT;
	} else {
		wxr_http_set_response(404, "Resource not found", NULL, 0, 0);
		return -EINVAL;
	}

	line = next + 2;
	while (*line) {
		next = strstr(line, "\r\n");
		if (!next)
			return -EINVAL;
		*next = '\0';
		if (!*line)
			break;

		if (!strncasecmp(line, "Content-Length:", 15)) {
			content_length_count++;
			ret = wxr_http_parse_content_length(line + 15,
						    &wxr_http.content_length);
			if (ret == -EFBIG) {
				wxr_http_set_response(413, "Upload exceeds 128 MiB",
						      NULL, 0, 0);
				return ret;
			}
			if (ret) {
				wxr_http_set_response(400, "Invalid Content-Length",
						      NULL, 0, 0);
				return ret;
			}
		} else if (!strncasecmp(line, "Content-Type:", 13)) {
			char *value = line + 13;

			while (*value == ' ' || *value == '\t')
				value++;
			content_type_ok = !strcasecmp(value,
						       "application/octet-stream");
		} else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
			transfer_encoding = 1;
		}
		line = next + 2;
	}

	if (transfer_encoding) {
		wxr_http_set_response(400, "Transfer-Encoding is not supported",
				      NULL, 0, 0);
		return -EINVAL;
	}
	if (content_length_count > 1) {
		wxr_http_set_response(400, "Duplicate Content-Length", NULL, 0, 0);
		return -EINVAL;
	}
	if (wxr_http.operation == WXR_HTTP_PAGE) {
		wxr_http_set_response(200, NULL, wxr_web_page_begin,
				      wxr_web_page_end - wxr_web_page_begin, 1);
		return 0;
	}
	if (wxr_http.operation == WXR_HTTP_REBOOT ||
	    wxr_http.operation == WXR_HTTP_RESET_NAND_CONFIGURATION ||
	    wxr_http.operation == WXR_HTTP_RESET_USB_CONFIGURATION) {
		if (content_length_count && wxr_http.content_length) {
			wxr_http_set_response(400, "Request body must be empty",
					      NULL, 0, 0);
			return -EINVAL;
		}
		wxr_http.phase = WXR_HTTP_EXECUTE;
		return 0;
	}
	if (!content_length_count) {
		wxr_http_set_response(411, "Content-Length is required", NULL, 0, 0);
		return -EINVAL;
	}
	if (!wxr_http.content_length) {
		wxr_http_set_response(400, "Upload must not be empty", NULL, 0, 0);
		return -EINVAL;
	}
	if (!content_type_ok) {
		wxr_http_set_response(415, "Use application/octet-stream",
				      NULL, 0, 0);
		return -EINVAL;
	}

	wxr_http.phase = WXR_HTTP_BODY;
	return 0;
}

static void wxr_http_execute(void)
{
	struct wxr_image image;
	enum wxr_error_kind kind;
	const char *error;
	const char *success = "Operation completed and verified";
	int status;
	int ret;

	wxr_error_clear();
	if (wxr_http.operation == WXR_HTTP_REBOOT) {
		puts("WXR web: reboot requested\n");
		run_command("reset", 0);
		wxr_error_set(WXR_ERROR_BOOT, "Router reboot", "reset handoff",
			      -EIO, 0);
		wxr_http_set_response(500, wxr_error_get(NULL), NULL, 0, 0);
		return;
	}
	if (wxr_http.operation == WXR_HTTP_RESET_NAND_CONFIGURATION) {
		tcp_stream_restart_rx_timer(wxr_http.tcp);
		ret = wxr_nand_reset_configuration();
		success = "NAND configuration was reset";
		goto complete;
	}
	if (wxr_http.operation == WXR_HTTP_RESET_USB_CONFIGURATION) {
		tcp_stream_restart_rx_timer(wxr_http.tcp);
		ret = wxr_usb_reset_configuration();
		success = "USB configuration was invalidated; ext4 will be rebuilt "
			  "on the next USB Production boot";
		goto complete;
	}

	ret = wxr_set_input(wxr_http.content_length, &image);
	if (ret) {
		wxr_http_set_response(422, wxr_error_get(NULL), NULL, 0, 0);
		return;
	}

	wxr_set_status(WXR_STATUS_VALIDATING);
	tcp_stream_restart_rx_timer(wxr_http.tcp);
	switch (wxr_http.operation) {
	case WXR_HTTP_BOOT_RECOVERY:
		ret = wxr_boot_recovery(wxr_web_cmdtp, &image);
		break;
	case WXR_HTTP_WRITE_NAND_RECOVERY:
		ret = wxr_nand_write_recovery(&image);
		break;
	case WXR_HTTP_WRITE_NAND_PRODUCTION:
		ret = wxr_nand_write_production(&image);
		break;
	case WXR_HTTP_WRITE_USB_RECOVERY:
		ret = wxr_usb_write_recovery(&image);
		break;
	case WXR_HTTP_WRITE_USB_PRODUCTION:
		ret = wxr_usb_write_production(&image);
		break;
	default:
		ret = -EINVAL;
		wxr_error_set(WXR_ERROR_INTERNAL, "Web recovery",
			      "operation selection", ret, 0);
		break;
	}

complete:
	tcp_stream_restart_rx_timer(wxr_http.tcp);

	if (ret) {
		wxr_set_status(WXR_STATUS_FAILURE);
		error = wxr_error_get(&kind);
		switch (kind) {
		case WXR_ERROR_IMAGE:
			status = 422;
			break;
		case WXR_ERROR_TARGET:
			status = 409;
			break;
		case WXR_ERROR_CAPACITY:
			status = 507;
			break;
		default:
			status = 500;
			break;
		}
		printf("WXR web: %s\n", error);
		wxr_http_set_response(status, error, NULL, 0, 0);
		return;
	}
	wxr_http_set_response(200, success, NULL, 0, 0);
}

static int wxr_http_on_create(struct tcp_stream *tcp)
{
	if (tcp->lport != WXR_WEB_PORT || wxr_http.tcp)
		return 0;

	wxr_http_reset_request();
	wxr_http.tcp = tcp;
	tcp->priv = &wxr_http;
	tcp->rx = wxr_http_rx;
	tcp->tx = wxr_http_tx;
	tcp->on_rcv_nxt_update = wxr_http_on_rcv_nxt_update;
	tcp->on_snd_una_update = wxr_http_on_snd_una_update;
	tcp->on_closed = wxr_http_on_closed;
	memset(net_server_ethaddr, 0, sizeof(net_server_ethaddr));
	return 1;
}

static int wxr_http_rx(struct tcp_stream *tcp, u32 rx_offs, void *buf, int len)
{
	size_t i;
	size_t body_offset;
	size_t body_length;

	if (tcp != wxr_http.tcp)
		return -EINVAL;
	if (wxr_http.phase == WXR_HTTP_RESPONSE)
		return len;

	if (!wxr_http.header_length) {
		if (rx_offs != wxr_http.header_received)
			return 0;
		if (wxr_http.header_received + len > WXR_HTTP_HEADER_MAX) {
			wxr_http_set_response(431, "Request headers exceed 4096 bytes",
					      NULL, 0, 0);
			return len;
		}

		memcpy(wxr_http.header + wxr_http.header_received, buf, len);
		wxr_http.header_received += len;
		wxr_http.header[wxr_http.header_received] = '\0';
		for (i = 3; i < wxr_http.header_received; i++) {
			if (!memcmp(wxr_http.header + i - 3, "\r\n\r\n", 4)) {
				wxr_http.header_length = i + 1;
				break;
			}
		}
		if (!wxr_http.header_length)
			return len;

		wxr_http_parse_request();
		if (wxr_http.phase == WXR_HTTP_EXECUTE) {
			wxr_http_execute();
			return len;
		}
		if (wxr_http.phase != WXR_HTTP_BODY)
			return len;

		body_offset = wxr_http.header_length - rx_offs;
		body_length = len - body_offset;
		if (body_length > wxr_http.content_length) {
			wxr_http_set_response(400, "Request body exceeds Content-Length",
					      NULL, 0, 0);
			return len;
		}
		memcpy((void *)WXR_INPUT_ADDRESS, (char *)buf + body_offset,
		       body_length);
		return len;
	}

	if (rx_offs < wxr_http.header_length)
		return 0;
	body_offset = rx_offs - wxr_http.header_length;
	if (body_offset > wxr_http.content_length ||
	    (size_t)len > wxr_http.content_length - body_offset) {
		wxr_http_set_response(400, "Request body exceeds Content-Length",
				      NULL, 0, 0);
		return len;
	}
	memcpy((void *)(WXR_INPUT_ADDRESS + body_offset), buf, len);
	return len;
}

static int wxr_http_tx(struct tcp_stream *tcp, u32 tx_offs, void *buf,
		       int maxlen)
{
	size_t available;
	size_t copied;

	if (tcp != wxr_http.tcp || wxr_http.phase != WXR_HTTP_RESPONSE)
		return 0;
	if (tx_offs >= wxr_http.response_total)
		return 0;

	available = wxr_http.response_total - tx_offs;
	if (available > (size_t)maxlen)
		available = maxlen;
	copied = 0;
	if (tx_offs < wxr_http.response_header_length) {
		size_t header_bytes = wxr_http.response_header_length - tx_offs;

		if (header_bytes > available)
			header_bytes = available;
		memcpy(buf, wxr_http.response_header + tx_offs, header_bytes);
		copied = header_bytes;
	}
	if (copied < available) {
		size_t body_offset = tx_offs + copied -
				     wxr_http.response_header_length;

		memcpy((char *)buf + copied,
		       (const char *)wxr_http.response_body + body_offset,
		       available - copied);
	}
	return available;
}

static void wxr_http_on_rcv_nxt_update(struct tcp_stream *tcp, u32 rx_bytes)
{
	if (tcp != wxr_http.tcp || wxr_http.phase != WXR_HTTP_BODY)
		return;
	if (rx_bytes == wxr_http.header_length + wxr_http.content_length) {
		wxr_http.phase = WXR_HTTP_EXECUTE;
		wxr_http_execute();
	}
}

static void wxr_http_on_snd_una_update(struct tcp_stream *tcp, u32 tx_bytes)
{
	if (tcp == wxr_http.tcp && wxr_http.phase == WXR_HTTP_RESPONSE &&
	    tx_bytes >= wxr_http.response_total)
		tcp_stream_close(tcp);
}

static void wxr_http_on_closed(struct tcp_stream *tcp)
{
	if (tcp == wxr_http.tcp)
		wxr_http_reset_request();
}

void wxr_web_start_server(void)
{
	wxr_http_reset_request();
	tcp_stream_set_on_create_handler(wxr_http_on_create);
}

int wxr_web_fixed(cmd_tbl_t *cmdtp)
{
	struct in_addr saved_ip = net_ip;
	struct in_addr saved_netmask = net_netmask;
	int ret;

	net_ip = string_to_ip("192.168.11.1");
	net_netmask = string_to_ip("255.255.255.0");
	wxr_web_cmdtp = cmdtp;
	wxr_set_status(WXR_STATUS_WAITING);
	puts("WXR fixed-IP web recovery\n"
	     "Connect a computer to Ethernet, set it to 192.168.11.2/24,\n"
	     "then open http://192.168.11.1/. Press Ctrl-C for the console.\n");
	ret = net_loop(WXR_WEB);
	net_ip = saved_ip;
	net_netmask = saved_netmask;
	wxr_web_cmdtp = NULL;
	if (ret == -EINTR) {
		wxr_set_status(WXR_STATUS_READY);
		return 0;
	}
	if (!ret)
		ret = -EIO;
	wxr_error_set(WXR_ERROR_IO, "Fixed-IP Web",
		      ret == -ENETDOWN ?
		      "Ethernet link readiness" : "network service",
		      ret, 0);
	wxr_set_status(WXR_STATUS_FAILURE);
	return ret;
}

int wxr_web_dhcp(cmd_tbl_t *cmdtp)
{
	struct wxr_saved_env saved_env[] = {
		{ "hostname", NULL, 0 },
		{ "autoload", NULL, 0 },
		{ "netretry", NULL, 0 },
		{ "bootfile", NULL, 0 },
	};
	struct wxr_saved_network saved_network;
	const char *value;
	int saved_count = 0;
	int restore_error = 0;
	int env_error;
	int ret = 0;
	int i;

	saved_network.ip = net_ip;
	saved_network.netmask = net_netmask;
	saved_network.gateway = net_gateway;
	saved_network.dns_server = net_dns_server;
#ifdef CONFIG_BOOTP_DNS2
	saved_network.dns_server2 = net_dns_server2;
#endif
	saved_network.server_ip = net_server_ip;
	memcpy(saved_network.server_ethaddr, net_server_ethaddr,
	       sizeof(saved_network.server_ethaddr));
	memcpy(saved_network.boot_file_name, net_boot_file_name,
	       sizeof(saved_network.boot_file_name));
	memcpy(saved_network.hostname, net_hostname,
	       sizeof(saved_network.hostname));

	for (i = 0; i < ARRAY_SIZE(saved_env); i++) {
		value = getenv(saved_env[i].name);
		saved_env[i].existed = value != NULL;
		if (value) {
			saved_env[i].value = strdup(value);
			if (!saved_env[i].value) {
				ret = -ENOMEM;
				wxr_error_set(WXR_ERROR_INTERNAL, "DHCP Web",
					      "environment snapshot", ret, 0);
				wxr_set_status(WXR_STATUS_FAILURE);
				goto restore;
			}
		}
		saved_count++;
	}

	if (setenv("hostname", WXR_DHCP_HOSTNAME) ||
	    setenv("autoload", "no") ||
	    setenv("netretry", "no") ||
	    (getenv("bootfile") && setenv("bootfile", NULL))) {
		ret = -ENOMEM;
		wxr_error_set(WXR_ERROR_INTERNAL, "DHCP Web",
			      "temporary environment", ret, 0);
		wxr_set_status(WXR_STATUS_FAILURE);
		goto restore;
	}

	net_ip.s_addr = 0;
	net_netmask.s_addr = 0;
	net_gateway.s_addr = 0;
	net_dns_server.s_addr = 0;
#ifdef CONFIG_BOOTP_DNS2
	net_dns_server2.s_addr = 0;
#endif
	net_server_ip.s_addr = 0;
	memset(net_server_ethaddr, 0, sizeof(net_server_ethaddr));
	net_boot_file_name[0] = '\0';
	net_hostname[0] = '\0';

	wxr_set_status(WXR_STATUS_WAITING);
	puts("WXR DHCP web recovery: requesting an address. "
	     "Press Ctrl-C for the console.\n");
	ret = net_loop(DHCP);
	if (ret == -EINTR) {
		wxr_set_status(WXR_STATUS_READY);
		ret = 0;
		goto restore;
	}
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "DHCP Web",
			      ret == -ENETDOWN ?
			      "Ethernet link readiness" :
			      "DHCP lease acquisition",
			      ret, 0);
		wxr_set_status(WXR_STATUS_FAILURE);
		goto restore;
	}
	if (!net_ip.s_addr || !net_netmask.s_addr) {
		ret = -EINVAL;
		wxr_error_set(WXR_ERROR_IO, "DHCP Web",
			      "DHCP lease validation", ret, 0);
		wxr_set_status(WXR_STATUS_FAILURE);
		goto restore;
	}

	printf("WXR DHCP web recovery: open http://%pI4/. "
	       "Press Ctrl-C for the console.\n", &net_ip);
	wxr_web_cmdtp = cmdtp;
	ret = net_loop(WXR_WEB);
	wxr_web_cmdtp = NULL;
	if (ret == -EINTR) {
		wxr_set_status(WXR_STATUS_READY);
		ret = 0;
	} else {
		if (!ret)
			ret = -EIO;
		wxr_error_set(WXR_ERROR_IO, "DHCP Web", "network service",
			      ret, 0);
		wxr_set_status(WXR_STATUS_FAILURE);
	}

restore:
	for (i = 0; i < saved_count; i++) {
		env_error = 0;
		if (saved_env[i].existed)
			env_error = setenv(saved_env[i].name,
					   saved_env[i].value);
		else if (getenv(saved_env[i].name))
			env_error = setenv(saved_env[i].name, NULL);
		if (env_error)
			restore_error = -ENOMEM;
		free(saved_env[i].value);
	}

	net_ip = saved_network.ip;
	net_netmask = saved_network.netmask;
	net_gateway = saved_network.gateway;
	net_dns_server = saved_network.dns_server;
#ifdef CONFIG_BOOTP_DNS2
	net_dns_server2 = saved_network.dns_server2;
#endif
	net_server_ip = saved_network.server_ip;
	memcpy(net_server_ethaddr, saved_network.server_ethaddr,
	       sizeof(net_server_ethaddr));
	memcpy(net_boot_file_name, saved_network.boot_file_name,
	       sizeof(net_boot_file_name));
	memcpy(net_hostname, saved_network.hostname,
	       sizeof(net_hostname));

	if (restore_error) {
		wxr_error_set(WXR_ERROR_INTERNAL, "DHCP Web",
			      "environment restoration", restore_error, 0);
		wxr_set_status(WXR_STATUS_FAILURE);
		ret = restore_error;
	}
	return ret;
}
