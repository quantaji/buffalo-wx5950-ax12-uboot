/*
 * Buffalo WXR-5950AX12 fixed-address TFTP recovery client.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <malloc.h>
#include <net.h>
#include <asm/cache.h>

#include "wxr5950ax12_boot.h"

#define WXR_TFTP_LOCAL_IP		"192.168.11.1"
#define WXR_TFTP_NETMASK		"255.255.255.0"
#define WXR_TFTP_SERVER_IP		"192.168.11.10"
#define WXR_TFTP_FILENAME					\
	"openwrt-qualcommax-ipq807x-buffalo_wxr-5950ax12-" \
	"initramfs-uImage.itb"
#define WXR_TFTP_TIMEOUT_MS		1000UL
#define WXR_TFTP_TIMEOUT_COUNT		60

struct wxr_tftp_saved_state {
	struct in_addr ip;
	struct in_addr netmask;
	struct in_addr gateway;
	struct in_addr server_ip;
	u8 server_ethaddr[6];
	char boot_file_name[sizeof(net_boot_file_name)];
	u32 boot_file_size;
	u32 expected_blocks;
	ulong load_address;
	u32 receive_size_limit;
	ulong timeout_ms;
	int timeout_count;
};

struct wxr_tftp_saved_env {
	const char *name;
	char *value;
	int existed;
};

int wxr_tftp_boot_recovery(cmd_tbl_t *cmdtp)
{
	struct wxr_tftp_saved_env saved_env[] = {
		{ "netretry", NULL, 0 },
		{ "tftptimeout", NULL, 0 },
		{ "tftptimeoutcountmax", NULL, 0 },
		{ "filesize", NULL, 0 },
		{ "fileaddr", NULL, 0 },
	};
	struct wxr_tftp_saved_state saved;
	struct wxr_image image;
	const char *value;
	int saved_count = 0;
	int restore_error = 0;
	int received = 0;
	int env_error;
	int transfer_attempted = 0;
	int ret = 0;
	int i;

	saved.ip = net_ip;
	saved.netmask = net_netmask;
	saved.gateway = net_gateway;
	saved.server_ip = net_server_ip;
	memcpy(saved.server_ethaddr, net_server_ethaddr,
	       sizeof(saved.server_ethaddr));
	memcpy(saved.boot_file_name, net_boot_file_name,
	       sizeof(saved.boot_file_name));
	saved.boot_file_size = net_boot_file_size;
	saved.expected_blocks = net_boot_file_expected_size_in_blocks;
	saved.load_address = load_addr;
	saved.receive_size_limit = tftp_receive_size_limit;
	saved.timeout_ms = tftp_timeout_ms;
	saved.timeout_count = tftp_timeout_count_max;

	for (i = 0; i < ARRAY_SIZE(saved_env); i++) {
		value = getenv(saved_env[i].name);
		saved_env[i].existed = value != NULL;
		if (value) {
			saved_env[i].value = strdup(value);
			if (!saved_env[i].value) {
				ret = -ENOMEM;
				wxr_error_set(WXR_ERROR_INTERNAL, "TFTP recovery",
					      "environment snapshot", ret, 0);
				goto restore;
			}
		}
		saved_count++;
	}

	net_ip = string_to_ip(WXR_TFTP_LOCAL_IP);
	net_netmask = string_to_ip(WXR_TFTP_NETMASK);
	net_gateway.s_addr = 0;
	net_server_ip = string_to_ip(WXR_TFTP_SERVER_IP);
	memset(net_server_ethaddr, 0, sizeof(net_server_ethaddr));
	copy_filename(net_boot_file_name, WXR_TFTP_FILENAME,
		      sizeof(net_boot_file_name));
	net_boot_file_size = 0;
	net_boot_file_expected_size_in_blocks = 0;
	load_addr = WXR_INPUT_ADDRESS;
	tftp_receive_size_limit = WXR_INPUT_MAX_SIZE;
	tftp_timeout_ms = WXR_TFTP_TIMEOUT_MS;
	tftp_timeout_count_max = WXR_TFTP_TIMEOUT_COUNT;

	env_error = setenv("netretry", "no");
	if (!env_error && getenv("tftptimeout"))
		env_error = setenv("tftptimeout", NULL);
	if (!env_error && getenv("tftptimeoutcountmax"))
		env_error = setenv("tftptimeoutcountmax", NULL);
	if (env_error) {
		ret = -EIO;
		wxr_error_set(WXR_ERROR_INTERNAL, "TFTP recovery",
			      "temporary environment", ret, 0);
		goto restore;
	}

	printf("WXR TFTP recovery: local %s, server %s\n",
	       WXR_TFTP_LOCAL_IP, WXR_TFTP_SERVER_IP);
	printf("WXR TFTP recovery: requesting '%s'\n", WXR_TFTP_FILENAME);
	printf("WXR TFTP recovery: load 0x%08lx, limit %lu bytes; "
	       "Ctrl-C cancels\n", WXR_INPUT_ADDRESS, WXR_INPUT_MAX_SIZE);

	wxr_set_status(WXR_STATUS_WAITING);
	transfer_attempted = 1;
	received = net_loop(TFTPGET);
	if (received > 0)
		flush_cache(WXR_INPUT_ADDRESS, received);
	ret = received;

restore:
	for (i = 0; i < saved_count; i++) {
		env_error = 0;
		if (saved_env[i].existed)
			env_error = setenv(saved_env[i].name,
					   saved_env[i].value);
		else if (getenv(saved_env[i].name))
			env_error = setenv(saved_env[i].name, NULL);
		if (env_error)
			restore_error = -EIO;
		free(saved_env[i].value);
	}

	net_ip = saved.ip;
	net_netmask = saved.netmask;
	net_gateway = saved.gateway;
	net_server_ip = saved.server_ip;
	memcpy(net_server_ethaddr, saved.server_ethaddr,
	       sizeof(net_server_ethaddr));
	memcpy(net_boot_file_name, saved.boot_file_name,
	       sizeof(net_boot_file_name));
	net_boot_file_size = saved.boot_file_size;
	net_boot_file_expected_size_in_blocks = saved.expected_blocks;
	load_addr = saved.load_address;
	tftp_receive_size_limit = saved.receive_size_limit;
	tftp_timeout_ms = saved.timeout_ms;
	tftp_timeout_count_max = saved.timeout_count;

	if (restore_error) {
		wxr_error_set(WXR_ERROR_INTERNAL, "TFTP recovery",
			      "environment restoration", restore_error, 0);
		return restore_error;
	}
	if (ret == -EINTR) {
		puts("WXR TFTP recovery: cancelled\n");
		wxr_set_status(WXR_STATUS_READY);
		return 0;
	}
	if (ret < 0) {
		if (transfer_attempted)
			wxr_error_set(WXR_ERROR_IO, "TFTP recovery",
				      "TFTP transfer", ret, 0);
		return ret;
	}
	if (!received) {
		wxr_error_set(WXR_ERROR_IMAGE, "TFTP recovery",
			      "image length", -EINVAL, 0);
		return -EINVAL;
	}
	if (received > WXR_INPUT_MAX_SIZE) {
		wxr_error_set(WXR_ERROR_CAPACITY, "TFTP recovery",
			      "image length", -E2BIG, 0);
		return -E2BIG;
	}

	ret = wxr_set_input((size_t)received, &image);
	if (ret)
		return ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	return wxr_boot_recovery(cmdtp, &image);
}
