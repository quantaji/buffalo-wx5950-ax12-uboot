/*
 * Buffalo WXR-5950AX12 physical selector, menu and LED status interface.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <asm/gpio.h>

#include "wxr5950ax12_boot.h"

#define WXR_MENU_DELAY	"2"
#define WXR_MENU_TITLE	\
	"\e[0;34m( ( ( \e[1;39mOpenWrt\e[0;34m ) ) ) " \
	"\e[0;36m[Buffalo WXR-5950AX12]\e[0m"

enum wxr_action {
	WXR_ACTION_NAND_PRODUCTION,
	WXR_ACTION_NAND_RECOVERY,
	WXR_ACTION_USB_PRODUCTION,
	WXR_ACTION_USB_RECOVERY,
	WXR_ACTION_FAT_RECOVERY,
	WXR_ACTION_TFTP_RECOVERY,
	WXR_ACTION_WEB_FIXED,
	WXR_ACTION_WEB_DHCP,
	WXR_ACTION_REBOOT,
	WXR_ACTION_CONSOLE,
};

struct wxr_menu_entry {
	const char *title;
	const char *command;
};

struct wxr_leds {
	int router_white;
	int router_red;
	int power_white;
	int power_red;
	int internet_white;
	int internet_red;
	int wireless_white;
	int wireless_red;
};

static const struct wxr_menu_entry wxr_menu_entries[] = {
	{ "Boot production system from NAND.",
	  "wxr_action nand-production" },
	{ "Boot recovery system from NAND.",
	  "wxr_action nand-recovery" },
	{ "Boot production system from USB storage.",
	  "wxr_action usb-production" },
	{ "Boot recovery system from the USB recovery partition.",
	  "wxr_action usb-recovery" },
	{ "Boot recovery system from a FAT-formatted USB device.",
	  "wxr_action fat-recovery" },
	{ "Boot recovery system via TFTP.",
	  "wxr_action tftp-recovery" },
	{ "Start web recovery with a fixed IP address.",
	  "wxr_action web-fixed" },
	{ "Start web recovery via DHCP.",
	  "wxr_action web-dhcp" },
	{ "Reboot.", "wxr_action reboot" },
};

static struct wxr_leds wxr_leds;
static enum wxr_status wxr_status;
static ulong wxr_status_time;
static int wxr_status_phase;
static int wxr_led_initialized;
static int wxr_wps_pressed;
static int wxr_input_failed;

extern int name_to_gpio(const char *name);

static int wxr_configure_led(const char *name, int *gpio)
{
	*gpio = name_to_gpio(name);
	if (*gpio < 0)
		return *gpio;
	return gpio_direction_output(*gpio, 0);
}

static int wxr_initialize_leds(void)
{
	if (wxr_configure_led("led-router-white", &wxr_leds.router_white) ||
	    wxr_configure_led("led-router-red", &wxr_leds.router_red) ||
	    wxr_configure_led("led-power-white", &wxr_leds.power_white) ||
	    wxr_configure_led("led-power-red", &wxr_leds.power_red) ||
	    wxr_configure_led("led-internet-white",
			      &wxr_leds.internet_white) ||
	    wxr_configure_led("led-internet-red", &wxr_leds.internet_red) ||
	    wxr_configure_led("led-wlan-white", &wxr_leds.wireless_white) ||
	    wxr_configure_led("led-wlan-red", &wxr_leds.wireless_red)) {
		puts("WXR UI: LED GPIO configuration failed\n");
		return -EIO;
	}

	wxr_led_initialized = 1;
	return 0;
}

static void wxr_apply_power_phase(void)
{
	int white = 0;
	int red = 0;

	if (!wxr_led_initialized)
		return;

	switch (wxr_status) {
	case WXR_STATUS_READY:
	case WXR_STATUS_SUCCESS:
		white = 1;
		break;
	case WXR_STATUS_WAITING:
	case WXR_STATUS_RECEIVING:
		white = !wxr_status_phase;
		break;
	case WXR_STATUS_VALIDATING:
		white = !wxr_status_phase;
		red = wxr_status_phase;
		break;
	case WXR_STATUS_WRITING:
		red = !wxr_status_phase;
		break;
	case WXR_STATUS_FAILURE:
		red = 1;
		break;
	}

	gpio_set_value(wxr_leds.power_white, 0);
	gpio_set_value(wxr_leds.power_red, 0);
	if (white)
		gpio_set_value(wxr_leds.power_white, 1);
	if (red)
		gpio_set_value(wxr_leds.power_red, 1);
}

void wxr_set_status(enum wxr_status status)
{
	wxr_status = status;
	wxr_status_phase = 0;
	wxr_status_time = get_timer(0);
	wxr_apply_power_phase();
}

void show_activity(int arg)
{
	ulong interval;
	ulong now;

	(void)arg;
	if (!wxr_led_initialized)
		return;

	switch (wxr_status) {
	case WXR_STATUS_WAITING:
		interval = 500;
		break;
	case WXR_STATUS_RECEIVING:
	case WXR_STATUS_VALIDATING:
	case WXR_STATUS_WRITING:
		interval = 125;
		break;
	default:
		return;
	}

	now = get_timer(0);
	if (now - wxr_status_time < interval)
		return;

	wxr_status_time = now;
	wxr_status_phase ^= 1;
	wxr_apply_power_phase();
}

static void wxr_disable_mode_leds(void)
{
	if (!wxr_led_initialized)
		return;

	gpio_set_value(wxr_leds.router_white, 0);
	gpio_set_value(wxr_leds.router_red, 0);
	gpio_set_value(wxr_leds.internet_white, 0);
	gpio_set_value(wxr_leds.internet_red, 0);
	gpio_set_value(wxr_leds.wireless_white, 0);
	gpio_set_value(wxr_leds.wireless_red, 0);
}

static int wxr_set_mode_leds(enum wxr_action action)
{
	wxr_disable_mode_leds();
	if (!wxr_led_initialized)
		return -EIO;

	switch (action) {
	case WXR_ACTION_NAND_PRODUCTION:
		gpio_set_value(wxr_leds.router_white, 1);
		break;
	case WXR_ACTION_NAND_RECOVERY:
		gpio_set_value(wxr_leds.router_red, 1);
		break;
	case WXR_ACTION_USB_PRODUCTION:
		gpio_set_value(wxr_leds.wireless_white, 1);
		break;
	case WXR_ACTION_USB_RECOVERY:
		gpio_set_value(wxr_leds.wireless_red, 1);
		break;
	case WXR_ACTION_FAT_RECOVERY:
		gpio_set_value(wxr_leds.router_white, 1);
		gpio_set_value(wxr_leds.wireless_white, 1);
		break;
	case WXR_ACTION_TFTP_RECOVERY:
		gpio_set_value(wxr_leds.internet_white, 1);
		break;
	case WXR_ACTION_WEB_FIXED:
		gpio_set_value(wxr_leds.router_white, 1);
		gpio_set_value(wxr_leds.internet_white, 1);
		break;
	case WXR_ACTION_WEB_DHCP:
		gpio_set_value(wxr_leds.wireless_white, 1);
		gpio_set_value(wxr_leds.internet_white, 1);
		break;
	case WXR_ACTION_REBOOT:
	case WXR_ACTION_CONSOLE:
		break;
	}

	return 0;
}

static int wxr_read_input(const char *name)
{
	int gpio = name_to_gpio(name);

	if (gpio < 0 || gpio_direction_input(gpio)) {
		printf("WXR UI: input '%s' cannot be configured\n", name);
		return -EIO;
	}

	return gpio_get_value(gpio);
}

static int wxr_select_action(enum wxr_action *action)
{
	int mode_ap = wxr_read_input("mode-ap");
	int mode_wb = wxr_read_input("mode-wb");
	int op_manual = wxr_read_input("op-manual");
	int reset = wxr_read_input("reset");

	if (mode_ap < 0 || mode_wb < 0 || op_manual < 0 || reset < 0)
		return -EIO;

	if (!mode_ap) {
		*action = op_manual ? WXR_ACTION_TFTP_RECOVERY :
				      WXR_ACTION_FAT_RECOVERY;
		return 0;
	}

	if (!mode_wb) {
		*action = op_manual ? WXR_ACTION_WEB_DHCP :
				      WXR_ACTION_WEB_FIXED;
		return 0;
	}

	if (op_manual)
		*action = reset ? WXR_ACTION_NAND_PRODUCTION :
				  WXR_ACTION_NAND_RECOVERY;
	else
		*action = reset ? WXR_ACTION_USB_PRODUCTION :
				  WXR_ACTION_USB_RECOVERY;

	return 0;
}

static int wxr_install_menu_entries(void)
{
	char name[16];
	char value[CONFIG_SYS_CBSIZE];
	int index;
	int length;

	for (index = 0; index < ARRAY_SIZE(wxr_menu_entries); index++) {
		snprintf(name, sizeof(name), "bootmenu_%d", index);
		length = snprintf(value, sizeof(value), "%s=%s",
				  wxr_menu_entries[index].title,
				  wxr_menu_entries[index].command);
		if (length < 0 || length >= (int)sizeof(value) ||
		    setenv(name, value))
			return -ENOMEM;
	}

	if ((getenv("bootmenu_9") && setenv("bootmenu_9", NULL)) ||
	    setenv("bootmenu_title", WXR_MENU_TITLE) ||
	    setenv("bootmenu_exit", "Exit to U-Boot console.") ||
	    setenv("bootmenu_delay", WXR_MENU_DELAY))
		return -ENOMEM;

	return 0;
}

static int wxr_set_menu_default(enum wxr_action action)
{
	char value[4];

	if (action < WXR_ACTION_NAND_PRODUCTION ||
	    action > WXR_ACTION_WEB_DHCP)
		return -EINVAL;

	snprintf(value, sizeof(value), "%d", action);
	return setenv("bootmenu_default", value) ? -ENOMEM : 0;
}

static int wxr_dispatch_action(cmd_tbl_t *cmdtp, enum wxr_action action)
{
	int ret;

	switch (action) {
	case WXR_ACTION_NAND_PRODUCTION:
		ret = wxr_nand_boot_production();
		break;
	case WXR_ACTION_NAND_RECOVERY:
		ret = wxr_nand_boot_recovery(cmdtp);
		break;
	case WXR_ACTION_USB_PRODUCTION:
		ret = wxr_usb_boot_production(cmdtp);
		break;
	case WXR_ACTION_USB_RECOVERY:
		ret = wxr_usb_boot_recovery(cmdtp);
		break;
	case WXR_ACTION_FAT_RECOVERY:
		puts("WXR boot menu: FAT recovery is unavailable\n");
		ret = -ENOSYS;
		break;
	case WXR_ACTION_TFTP_RECOVERY:
		puts("WXR boot menu: TFTP recovery is unavailable\n");
		ret = -ENOSYS;
		break;
	case WXR_ACTION_WEB_FIXED:
		puts("WXR boot menu: fixed-IP web recovery is unavailable\n");
		ret = -ENOSYS;
		break;
	case WXR_ACTION_WEB_DHCP:
		puts("WXR boot menu: DHCP web recovery is unavailable\n");
		ret = -ENOSYS;
		break;
	case WXR_ACTION_REBOOT:
		run_command("reset", 0);
		puts("WXR boot menu: reset returned\n");
		ret = -EIO;
		break;
	case WXR_ACTION_CONSOLE:
		wxr_set_status(WXR_STATUS_READY);
		return 0;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		printf("WXR boot menu: action failed (%d)\n", ret);
	if (ret)
		wxr_set_status(WXR_STATUS_FAILURE);
	return ret;
}

static int wxr_action_from_name(const char *name, enum wxr_action *action)
{
	static const char *const names[] = {
		"nand-production",
		"nand-recovery",
		"usb-production",
		"usb-recovery",
		"fat-recovery",
		"tftp-recovery",
		"web-fixed",
		"web-dhcp",
		"reboot",
		"console",
	};
	int index;

	for (index = 0; index < ARRAY_SIZE(names); index++)
		if (!strcmp(name, names[index])) {
			*action = index;
			return 0;
		}

	return -EINVAL;
}

static int do_wxr_action(cmd_tbl_t *cmdtp, int flag, int argc,
			 char *const argv[])
{
	enum wxr_action action;
	int ret;

	(void)flag;
	if (argc != 2 || wxr_action_from_name(argv[1], &action))
		return CMD_RET_USAGE;

	ret = wxr_dispatch_action(cmdtp, action);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_wxr_bootmenu(cmd_tbl_t *cmdtp, int flag, int argc,
			   char *const argv[])
{
	(void)cmdtp;
	(void)flag;
	(void)argc;
	(void)argv;

	if (wxr_wps_pressed) {
		puts("WXR boot menu: WPS pressed, entering U-Boot console\n");
		wxr_set_status(WXR_STATUS_READY);
		return CMD_RET_SUCCESS;
	}

	if (wxr_input_failed) {
		puts("WXR boot menu: physical input or UI initialization failed\n");
		wxr_set_status(WXR_STATUS_FAILURE);
		return CMD_RET_FAILURE;
	}

	wxr_set_status(WXR_STATUS_READY);
	return run_command("bootmenu " WXR_MENU_DELAY, 0);
}

void wxr_install_boot_flow(void)
{
	enum wxr_action action;
	int wps;

	if (setenv("bootdelay", "-1")) {
		puts("WXR UI: cannot disable the inherited boot command\n");
		return;
	}

	if (wxr_install_menu_entries() || wxr_initialize_leds())
		goto failure;

	wps = wxr_read_input("wps");
	if (wps < 0)
		goto failure;

	if (!wps) {
		wxr_wps_pressed = 1;
		wxr_set_status(WXR_STATUS_READY);
		if (setenv("preboot", "wxr_bootmenu"))
			goto failure;
		return;
	}

	if (wxr_select_action(&action) || wxr_set_mode_leds(action) ||
	    wxr_set_menu_default(action))
		goto failure;

	wxr_set_status(WXR_STATUS_READY);
	if (setenv("preboot", "wxr_bootmenu"))
		goto failure;
	return;

failure:
	wxr_input_failed = 1;
	wxr_disable_mode_leds();
	wxr_set_status(WXR_STATUS_FAILURE);
	if (setenv("preboot", "wxr_bootmenu"))
		puts("WXR UI: cannot install the safe preboot command\n");
}

U_BOOT_CMD(
	wxr_action, 2, 0, do_wxr_action,
	"dispatch one WXR-5950AX12 menu action",
	"nand-production|nand-recovery|usb-production|usb-recovery|\n"
	"wxr_action fat-recovery|tftp-recovery|web-fixed|web-dhcp|reboot|console"
);

U_BOOT_CMD(
	wxr_bootmenu, 1, 0, do_wxr_bootmenu,
	"enter the WXR-5950AX12 boot menu or selected console path",
	""
);
