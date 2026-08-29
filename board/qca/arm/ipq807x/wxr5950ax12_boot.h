#ifndef __WXR5950AX12_BOOT_H__
#define __WXR5950AX12_BOOT_H__

#include <common.h>
#include <command.h>
#include <u-boot/sha256.h>

#define WXR_INPUT_ADDRESS	0x50000000UL
#define WXR_INPUT_MAX_SIZE	(128UL << 20)
#define WXR_INPUT_END		(WXR_INPUT_ADDRESS + WXR_INPUT_MAX_SIZE)
#define WXR_KERNEL_ADDRESS	0x41000000UL
#define WXR_KERNEL_MAX_SIZE	(64UL << 20)
#define WXR_FIT_CONFIG		"config@hk01"

enum wxr_status {
	WXR_STATUS_READY,
	WXR_STATUS_WAITING,
	WXR_STATUS_RECEIVING,
	WXR_STATUS_VALIDATING,
	WXR_STATUS_WRITING,
	WXR_STATUS_SUCCESS,
	WXR_STATUS_FAILURE,
};

enum wxr_error_kind {
	WXR_ERROR_NONE,
	WXR_ERROR_IMAGE,
	WXR_ERROR_TARGET,
	WXR_ERROR_CAPACITY,
	WXR_ERROR_IO,
	WXR_ERROR_VERIFY,
	WXR_ERROR_BOOT,
	WXR_ERROR_INTERNAL,
};

enum wxr_sysupgrade_target {
	WXR_SYSUPGRADE_NAND,
	WXR_SYSUPGRADE_USB,
};

struct wxr_image {
	ulong address;
	size_t length;
};

struct wxr_member {
	const void *data;
	size_t length;
};

struct wxr_sysupgrade {
	struct wxr_member kernel;
	struct wxr_member root;
};

int wxr_set_input(size_t length, struct wxr_image *image);
int wxr_validate_recovery(const struct wxr_image *image);
int wxr_validate_openwrt_fit(const struct wxr_member *fit);
int wxr_validate_squashfs(const struct wxr_member *root, u64 *bytes_used);
int wxr_boot_recovery(cmd_tbl_t *cmdtp, const struct wxr_image *image);
int wxr_parse_sysupgrade(const struct wxr_image *image,
			 enum wxr_sysupgrade_target target,
			 struct wxr_sysupgrade *archive);
void wxr_sha256(const void *data, size_t length,
		u8 digest[SHA256_SUM_LEN]);
void wxr_error_clear(void);
void wxr_error_set(enum wxr_error_kind kind, const char *target,
		   const char *stage, int code, int target_changed);
const char *wxr_error_get(enum wxr_error_kind *kind);

int wxr_nand_boot_production(void);
int wxr_nand_boot_recovery(cmd_tbl_t *cmdtp);
int wxr_usb_boot_production(cmd_tbl_t *cmdtp);
int wxr_usb_boot_recovery(cmd_tbl_t *cmdtp);

int wxr_nand_write_recovery(const struct wxr_image *image);
int wxr_nand_write_production(const struct wxr_image *image);
int wxr_usb_write_recovery(const struct wxr_image *image);
int wxr_usb_write_production(const struct wxr_image *image);
int wxr_nand_reset_configuration(void);
int wxr_usb_reset_configuration(void);

int wxr_web_fixed(cmd_tbl_t *cmdtp);
int wxr_web_dhcp(cmd_tbl_t *cmdtp);
void wxr_web_start_server(void);

void wxr_set_status(enum wxr_status status);
void wxr_install_boot_flow(void);

#endif
