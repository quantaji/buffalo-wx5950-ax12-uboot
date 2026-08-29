/*
 * Buffalo WXR-5950AX12 image validation and recovery boot contracts.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <image.h>
#include <linux/ctype.h>
#include <u-boot/crc.h>
#include <u-boot/sha256.h>
#include <watchdog.h>

#include "wxr5950ax12_boot.h"

#define WXR_BOARD_COMPATIBLE		"buffalo,wxr-5950ax12"
#define WXR_TAR_BLOCK_SIZE		512
#define WXR_FWTOOL_MAGIC		0x46577830
#define WXR_FWTOOL_INFO			1
#define WXR_FWTOOL_METADATA_MAX_SIZE	(30UL << 10)
#define WXR_HASH_CHUNK_SIZE		(64UL << 10)
#define WXR_SQUASHFS_HEADER_SIZE	48
#define WXR_SQUASHFS_BYTES_USED_OFFSET	40
#define WXR_ERROR_TEXT_SIZE		192

static struct {
	enum wxr_error_kind kind;
	const char *target;
	const char *stage;
	int code;
	int target_changed;
	char text[WXR_ERROR_TEXT_SIZE];
} wxr_error;

struct wxr_fwtool_header {
	u32 version;
	u32 flags;
};

struct wxr_fwtool_trailer {
	u32 magic;
	u32 crc32;
	u8 type;
	u8 pad[3];
	u32 size;
};

struct wxr_tar_header {
	u8 name[100];
	u8 mode[8];
	u8 uid[8];
	u8 gid[8];
	u8 size[12];
	u8 mtime[12];
	u8 checksum[8];
	u8 type;
	u8 linkname[100];
	u8 magic[6];
	u8 version[2];
	u8 owner[32];
	u8 group[32];
	u8 major[8];
	u8 minor[8];
	u8 prefix[155];
	u8 padding[12];
};

void wxr_error_clear(void)
{
	memset(&wxr_error, 0, sizeof(wxr_error));
}

void wxr_error_set(enum wxr_error_kind kind, const char *target,
		   const char *stage, int code, int target_changed)
{
	wxr_error.kind = kind;
	wxr_error.target = target;
	wxr_error.stage = stage;
	wxr_error.code = code;
	wxr_error.target_changed = target_changed;
}

const char *wxr_error_get(enum wxr_error_kind *kind)
{
	static const char *const reasons[] = {
		[WXR_ERROR_NONE] = "operation failed without error context",
		[WXR_ERROR_IMAGE] = "image does not match the required contract",
		[WXR_ERROR_TARGET] = "target is missing, unavailable, or invalid",
		[WXR_ERROR_CAPACITY] = "target has insufficient capacity",
		[WXR_ERROR_IO] = "I/O operation failed",
		[WXR_ERROR_VERIFY] = "verification failed",
		[WXR_ERROR_BOOT] = "boot handoff failed",
		[WXR_ERROR_INTERNAL] = "internal operation failed",
	};
	const char *target = wxr_error.target ? wxr_error.target : "WXR operation";
	const char *stage = wxr_error.stage ? wxr_error.stage : "unknown stage";

	if (kind)
		*kind = wxr_error.kind;
	snprintf(wxr_error.text, sizeof(wxr_error.text),
		 "%s / %s: %s (%d); %s", target, stage,
		 reasons[wxr_error.kind], wxr_error.code,
		 wxr_error.target_changed ? "target may be incomplete" :
					    "persistent storage was not changed");
	return wxr_error.text;
}

static int wxr_range_contains(const void *container, size_t container_length,
			      const void *data, size_t data_length)
{
	ulong container_address = (ulong)container;
	ulong data_address = (ulong)data;

	if (data_address < container_address)
		return 0;

	data_address -= container_address;
	return data_address <= container_length &&
		data_length <= container_length - data_address;
}

static int wxr_image_has_hash(const void *fit, int image_node)
{
	int node;

	fdt_for_each_subnode(fit, node, image_node) {
		const char *name = fit_get_name(fit, node, NULL);

		if (name && !strncmp(name, FIT_HASH_NODENAME,
				     strlen(FIT_HASH_NODENAME)))
			return 1;
	}

	return 0;
}

void wxr_sha256(const void *data, size_t length,
		u8 digest[SHA256_SUM_LEN])
{
	const u8 *current = data;
	sha256_context context;

	sha256_starts(&context);
	while (length) {
		size_t chunk = min(length, (size_t)WXR_HASH_CHUNK_SIZE);

		sha256_update(&context, current, chunk);
		current += chunk;
		length -= chunk;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}
	sha256_finish(&context, digest);
}

int wxr_set_input(size_t length, struct wxr_image *image)
{
	if (!image || !length || length > WXR_INPUT_MAX_SIZE) {
		wxr_error_set(WXR_ERROR_IMAGE, "Staged image", "input length",
			      -EINVAL, 0);
		return -EINVAL;
	}

	image->address = WXR_INPUT_ADDRESS;
	image->length = length;
	return 0;
}

int wxr_validate_openwrt_fit(const struct wxr_member *image)
{
	const void *fit;
	const void *kernel_data;
	const void *fdt_data;
	size_t kernel_size;
	size_t fdt_size;
	ulong kernel_load;
	ulong kernel_entry;
	int config_node;
	int kernel_node;
	int fdt_node;

	if (!image || !image->data ||
	    image->length < sizeof(struct fdt_header) ||
	    image->length > WXR_INPUT_MAX_SIZE) {
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "input",
			      -EINVAL, 0);
		return -EINVAL;
	}

	fit = image->data;
	if (fdt_check_header(fit) || fdt_totalsize(fit) != image->length ||
	    !fit_check_format(fit)) {
		puts("WXR image: malformed OpenWrt FIT\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "FIT structure",
			      -EINVAL, 0);
		return -EINVAL;
	}

	config_node = fit_conf_get_node(fit, WXR_FIT_CONFIG);
	if (config_node < 0) {
		puts("WXR image: config@hk01 is missing\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "configuration",
			      -ENOENT, 0);
		return -ENOENT;
	}

	kernel_node = fit_conf_get_prop_node(fit, config_node,
					    FIT_KERNEL_PROP);
	fdt_node = fit_conf_get_prop_node(fit, config_node, FIT_FDT_PROP);
	if (kernel_node < 0 || fdt_node < 0) {
		puts("WXR image: configured kernel or device tree is missing\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "configuration",
			      -ENOENT, 0);
		return -ENOENT;
	}

	if (!fit_image_check_type(fit, kernel_node, IH_TYPE_KERNEL) ||
	    !fit_image_check_arch(fit, kernel_node, IH_ARCH_ARM64) ||
	    !fit_image_check_os(fit, kernel_node, IH_OS_LINUX) ||
	    !fit_image_check_comp(fit, kernel_node, IH_COMP_GZIP) ||
	    fit_image_get_load(fit, kernel_node, &kernel_load) ||
	    fit_image_get_entry(fit, kernel_node, &kernel_entry) ||
	    fit_image_get_data(fit, kernel_node, &kernel_data, &kernel_size) ||
	    kernel_load != WXR_KERNEL_ADDRESS ||
	    kernel_entry != WXR_KERNEL_ADDRESS || !kernel_size ||
	    !wxr_range_contains(fit, image->length, kernel_data, kernel_size)) {
		puts("WXR image: standard OpenWrt kernel contract is invalid\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "kernel contract",
			      -EINVAL, 0);
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, fdt_node, IH_TYPE_FLATDT) ||
	    !fit_image_check_arch(fit, fdt_node, IH_ARCH_ARM64) ||
	    !fit_image_check_comp(fit, fdt_node, IH_COMP_NONE) ||
	    fit_image_get_data(fit, fdt_node, &fdt_data, &fdt_size) ||
	    !fdt_size ||
	    !wxr_range_contains(fit, image->length, fdt_data, fdt_size) ||
	    fdt_check_header(fdt_data) || fdt_totalsize(fdt_data) != fdt_size ||
	    fdt_node_check_compatible(fdt_data, 0, WXR_BOARD_COMPATIBLE)) {
		puts("WXR image: standard OpenWrt device-tree contract is invalid\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT",
			      "WXR device-tree contract", -EINVAL, 0);
		return -EINVAL;
	}

	if (!wxr_image_has_hash(fit, kernel_node) ||
	    !wxr_image_has_hash(fit, fdt_node)) {
		puts("WXR image: kernel or device tree has no hash\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "hash presence",
			      -EINVAL, 0);
		return -EINVAL;
	}

	if (!fit_image_verify(fit, kernel_node) ||
	    !fit_image_verify(fit, fdt_node)) {
		puts("WXR image: kernel or device-tree hash failed\n");
		wxr_error_set(WXR_ERROR_IMAGE, "OpenWrt FIT", "component hashes",
			      -EIO, 0);
		return -EIO;
	}

	return 0;
}

int wxr_validate_recovery(const struct wxr_image *image)
{
	struct wxr_member fit;

	if (!image || image->address != WXR_INPUT_ADDRESS ||
	    !image->length || image->length > WXR_INPUT_MAX_SIZE) {
		wxr_error_set(WXR_ERROR_IMAGE, "Recovery image", "staged input",
			      -EINVAL, 0);
		return -EINVAL;
	}

	fit.data = (const void *)image->address;
	fit.length = image->length;
	return wxr_validate_openwrt_fit(&fit);
}

int wxr_validate_squashfs(const struct wxr_member *root, u64 *bytes_used)
{
	u64 encoded_bytes;
	u64 length;

	if (!root || !root->data || root->length < WXR_SQUASHFS_HEADER_SIZE) {
		wxr_error_set(WXR_ERROR_IMAGE, "Production rootfs", "input",
			      -EINVAL, 0);
		return -EINVAL;
	}

	if (memcmp(root->data, "hsqs", 4)) {
		puts("WXR image: root member is not little-endian SquashFS\n");
		wxr_error_set(WXR_ERROR_IMAGE, "Production rootfs",
			      "SquashFS header", -EINVAL, 0);
		return -EINVAL;
	}

	memcpy(&encoded_bytes,
	       (const u8 *)root->data + WXR_SQUASHFS_BYTES_USED_OFFSET,
	       sizeof(encoded_bytes));
	length = le64_to_cpu(encoded_bytes);
	if (length < WXR_SQUASHFS_HEADER_SIZE || length > root->length) {
		puts("WXR image: SquashFS bytes_used is outside the root member\n");
		wxr_error_set(WXR_ERROR_IMAGE, "Production rootfs",
			      "SquashFS length", -EINVAL, 0);
		return -EINVAL;
	}

	if (bytes_used)
		*bytes_used = length;
	return 0;
}

int wxr_boot_recovery(cmd_tbl_t *cmdtp, const struct wxr_image *image)
{
	char previous_bootargs[CONFIG_SYS_CBSIZE];
	char *bootm_argv[] = {
		"bootm",
		"0x50000000#config@hk01",
		NULL,
	};
	const char *current;
	int had_bootargs;
	int ret;

	ret = wxr_validate_recovery(image);
	if (ret)
		return ret;

	current = getenv("bootargs");
	had_bootargs = current != NULL;
	if (had_bootargs) {
		if (strlen(current) >= sizeof(previous_bootargs)) {
			puts("WXR recovery: existing bootargs cannot be preserved\n");
			wxr_error_set(WXR_ERROR_BOOT, "Recovery", "bootargs",
				      -E2BIG, 0);
			return -E2BIG;
		}
		strlcpy(previous_bootargs, current, sizeof(previous_bootargs));
	}

	if (setenv("bootargs", CONFIG_BOOTARGS)) {
		puts("WXR recovery: cannot set temporary bootargs\n");
		wxr_error_set(WXR_ERROR_BOOT, "Recovery", "bootargs",
			      -ENOMEM, 0);
		return -ENOMEM;
	}

	wxr_set_status(WXR_STATUS_SUCCESS);
	ret = do_bootm(cmdtp, 0, 2, bootm_argv);

	if (had_bootargs)
		setenv("bootargs", previous_bootargs);
	else
		setenv("bootargs", NULL);

	printf("WXR recovery: bootm returned %d\n", ret);
	wxr_set_status(WXR_STATUS_FAILURE);
	wxr_error_set(WXR_ERROR_BOOT, "Recovery", "bootm handoff", ret, 0);
	return -EIO;
}

static int wxr_block_is_zero(const u8 *data)
{
	int i;

	for (i = 0; i < WXR_TAR_BLOCK_SIZE; i++)
		if (data[i])
			return 0;

	return 1;
}

static int wxr_parse_tar_octal(const u8 *field, size_t field_length,
			       size_t *value)
{
	size_t result = 0;
	size_t index = 0;
	int have_digit = 0;

	while (index < field_length &&
	       (field[index] == ' ' || field[index] == '\0'))
		index++;

	while (index < field_length && field[index] >= '0' &&
	       field[index] <= '7') {
		if (result > (SIZE_MAX - 7) / 8)
			return -EOVERFLOW;
		result = result * 8 + field[index] - '0';
		have_digit = 1;
		index++;
	}

	while (index < field_length) {
		if (field[index] != ' ' && field[index] != '\0')
			return -EINVAL;
		index++;
	}

	if (!have_digit)
		return -EINVAL;

	*value = result;
	return 0;
}

static int wxr_verify_tar_header(const struct wxr_tar_header *header)
{
	const u8 *bytes = (const u8 *)header;
	size_t expected;
	size_t sum = 0;
	int is_gnu;
	int is_posix;
	int i;

	is_posix = !memcmp(header->magic, "ustar", 5) &&
		   header->magic[5] == '\0' &&
		   !memcmp(header->version, "00", 2);
	is_gnu = !memcmp(header->magic, "ustar ", 6) &&
		 header->version[0] == ' ' && header->version[1] == '\0';
	if (!is_posix && !is_gnu)
		return -EINVAL;

	if (wxr_parse_tar_octal(header->checksum,
				sizeof(header->checksum), &expected))
		return -EINVAL;

	for (i = 0; i < sizeof(*header); i++) {
		if (i >= offsetof(struct wxr_tar_header, checksum) &&
		    i < offsetof(struct wxr_tar_header, checksum) +
			    sizeof(header->checksum))
			sum += ' ';
		else
			sum += bytes[i];
	}

	return sum == expected ? 0 : -EINVAL;
}

static int wxr_tar_name_is(const struct wxr_tar_header *header,
			   const char *expected)
{
	size_t length = strlen(expected);

	return length < sizeof(header->name) &&
		!memcmp(header->name, expected, length) &&
		header->name[length] == '\0' && !header->prefix[0];
}

static int wxr_verify_fwtool(const struct wxr_image *image,
			     size_t *metadata_start)
{
	const struct wxr_fwtool_header *header;
	const struct wxr_fwtool_trailer *trailer;
	const u8 *data = (const u8 *)image->address;
	size_t chunk_size;
	u32 calculated;

	if (image->length < sizeof(*header) + sizeof(*trailer)) {
		wxr_error_set(WXR_ERROR_IMAGE, "Sysupgrade", "fwtool metadata",
			      -EINVAL, 0);
		return -EINVAL;
	}

	trailer = (const void *)(data + image->length - sizeof(*trailer));
	chunk_size = be32_to_cpu(trailer->size);
	if (be32_to_cpu(trailer->magic) != WXR_FWTOOL_MAGIC ||
	    trailer->type != WXR_FWTOOL_INFO || trailer->pad[0] ||
	    trailer->pad[1] || trailer->pad[2] ||
	    chunk_size < sizeof(*header) + sizeof(*trailer) ||
	    chunk_size > sizeof(*header) + WXR_FWTOOL_METADATA_MAX_SIZE +
			 sizeof(*trailer) || chunk_size > image->length) {
		puts("WXR sysupgrade: fwtool trailer is invalid\n");
		wxr_error_set(WXR_ERROR_IMAGE, "Sysupgrade", "fwtool metadata",
			      -EINVAL, 0);
		return -EINVAL;
	}

	*metadata_start = image->length - chunk_size;
	header = (const void *)(data + *metadata_start);
	if (header->version || header->flags) {
		puts("WXR sysupgrade: fwtool information header is invalid\n");
		wxr_error_set(WXR_ERROR_IMAGE, "Sysupgrade", "fwtool metadata",
			      -EINVAL, 0);
		return -EINVAL;
	}

	calculated = crc32_no_comp(~0U, data,
				   image->length - sizeof(*trailer));
	if (calculated != be32_to_cpu(trailer->crc32)) {
		puts("WXR sysupgrade: fwtool CRC32 mismatch\n");
		wxr_error_set(WXR_ERROR_IMAGE, "Sysupgrade", "fwtool CRC32",
			      -EIO, 0);
		return -EIO;
	}

	return 0;
}

static int wxr_read_tar_member(const u8 *data, size_t limit, size_t *offset,
			       const char *name, u8 type,
			       struct wxr_member *member)
{
	const struct wxr_tar_header *header;
	size_t padded_size;
	size_t size;

	if (*offset > limit || sizeof(*header) > limit - *offset)
		return -EINVAL;

	header = (const void *)(data + *offset);
	if (wxr_verify_tar_header(header) || !wxr_tar_name_is(header, name) ||
	    header->type != type ||
	    wxr_parse_tar_octal(header->size, sizeof(header->size), &size))
		return -EINVAL;

	if (size > SIZE_MAX - (WXR_TAR_BLOCK_SIZE - 1))
		return -EOVERFLOW;
	padded_size = ALIGN(size, WXR_TAR_BLOCK_SIZE);
	if (sizeof(*header) > limit - *offset ||
	    padded_size > limit - *offset - sizeof(*header))
		return -EINVAL;

	if (member) {
		member->data = data + *offset + sizeof(*header);
		member->length = size;
	}
	*offset += sizeof(*header) + padded_size;
	return 0;
}

int wxr_parse_sysupgrade(const struct wxr_image *image,
			 enum wxr_sysupgrade_target target,
			 struct wxr_sysupgrade *archive)
{
	const char *expected_directory;
	const char *expected_control;
	const char *target_name;
	const u8 *data;
	char control_name[128];
	char kernel_name[128];
	char root_name[128];
	struct wxr_member control;
	size_t metadata_start;
	size_t offset = 0;
	size_t index;
	int name_length;
	int ret;

	if (!image || !archive || image->address != WXR_INPUT_ADDRESS ||
	    !image->length || image->length > WXR_INPUT_MAX_SIZE) {
		wxr_error_set(WXR_ERROR_IMAGE, "Sysupgrade", "staged input",
			      -EINVAL, 0);
		return -EINVAL;
	}

	if (target == WXR_SYSUPGRADE_NAND) {
		expected_directory = "sysupgrade-buffalo_wxr-5950ax12/";
		expected_control = "BOARD=buffalo_wxr-5950ax12\n";
		target_name = "NAND sysupgrade";
	} else if (target == WXR_SYSUPGRADE_USB) {
		expected_directory = "sysupgrade-buffalo_wxr-5950ax12_usb/";
		expected_control = "BOARD=buffalo_wxr-5950ax12_usb\n";
		target_name = "USB sysupgrade";
	} else {
		wxr_error_set(WXR_ERROR_INTERNAL, "Sysupgrade", "target selection",
			      -EINVAL, 0);
		return -EINVAL;
	}

	ret = wxr_verify_fwtool(image, &metadata_start);
	if (ret)
		return ret;

	data = (const u8 *)image->address;
	name_length = snprintf(control_name, sizeof(control_name), "%sCONTROL",
			       expected_directory);
	if (name_length < 0 || name_length >= sizeof(control_name))
		return -E2BIG;
	name_length = snprintf(kernel_name, sizeof(kernel_name), "%skernel",
			       expected_directory);
	if (name_length < 0 || name_length >= sizeof(kernel_name))
		return -E2BIG;
	name_length = snprintf(root_name, sizeof(root_name), "%sroot",
			       expected_directory);
	if (name_length < 0 || name_length >= sizeof(root_name))
		return -E2BIG;

	ret = wxr_read_tar_member(data, metadata_start, &offset,
				  expected_directory, '5', NULL);
	if (ret)
		goto malformed;
	ret = wxr_read_tar_member(data, metadata_start, &offset,
				  control_name, '0', &control);
	if (ret)
		goto malformed;
	ret = wxr_read_tar_member(data, metadata_start, &offset,
				  kernel_name, '0', &archive->kernel);
	if (ret)
		goto malformed;
	ret = wxr_read_tar_member(data, metadata_start, &offset,
				  root_name, '0', &archive->root);
	if (ret)
		goto malformed;

	if (control.length != strlen(expected_control) ||
	    memcmp(control.data, expected_control, control.length) ||
	    !archive->kernel.length || !archive->root.length)
		goto malformed;

	if (offset > metadata_start ||
	    2 * WXR_TAR_BLOCK_SIZE > metadata_start - offset ||
	    !wxr_block_is_zero(data + offset) ||
	    !wxr_block_is_zero(data + offset + WXR_TAR_BLOCK_SIZE))
		goto malformed;

	for (index = offset + 2 * WXR_TAR_BLOCK_SIZE;
	     index < metadata_start; index++)
		if (data[index])
			goto malformed;

	return 0;

malformed:
	puts("WXR sysupgrade: archive members or boundaries are invalid\n");
	wxr_error_set(WXR_ERROR_IMAGE, target_name, "archive members",
		      -EINVAL, 0);
	memset(archive, 0, sizeof(*archive));
	return -EINVAL;
}
