/*
 * Buffalo WXR-5950AX12 raw USB boot and bounded write transactions.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <image.h>
#include <malloc.h>
#include <part.h>
#include <usb.h>
#include <asm/cache.h>
#include <u-boot/sha256.h>
#include <watchdog.h>

#include "wxr5950ax12_boot.h"

#define WXR_USB_RECOVERY_PARTITION	"wxr_recovery"
#define WXR_USB_PRODUCTION_PARTITION	"wxr_production"
#define WXR_USB_ROOTFS_PARTITION	"wxr_rootfs"
#define WXR_USB_OVERLAY_PARTITION	"rootfs_data"

#define WXR_USB_RECOVERY_START		2048ULL
#define WXR_USB_RECOVERY_BLOCKS		262144ULL
#define WXR_USB_PRODUCTION_START	264192ULL
#define WXR_USB_PRODUCTION_BLOCKS	262144ULL
#define WXR_USB_ROOTFS_START		526336ULL
#define WXR_USB_ROOTFS_BLOCKS		524288ULL
#define WXR_USB_OVERLAY_START		1050624ULL
#define WXR_USB_OVERLAY_MIN_BLOCKS	902466ULL

#define WXR_USB_CONTRACT_IMAGE		"contract@1"
#define WXR_USB_CONTRACT_COMPATIBLE	\
	"openwrt,wxr-5950ax12-boot-contract"
#define WXR_USB_CONTRACT_VERSION	1
#define WXR_USB_IO_BUFFER_SIZE		(64UL << 10)
#define WXR_USB_OVERLAY_CLEAR_SIZE	(1UL << 20)
#define WXR_BOARD_COMPATIBLE		"buffalo,wxr-5950ax12"

enum wxr_usb_role {
	WXR_USB_RECOVERY,
	WXR_USB_PRODUCTION,
};

struct wxr_usb_layout {
	int devnum;
	block_dev_desc_t *device;
	disk_partition_t recovery;
	disk_partition_t production;
	disk_partition_t rootfs;
	disk_partition_t overlay;
};

struct wxr_rootfs_contract {
	u64 bytes_used;
	u8 sha256[SHA256_SUM_LEN];
};

static u64 wxr_partition_bytes(const disk_partition_t *partition)
{
	return (u64)partition->size * partition->blksz;
}

static int wxr_partition_is_valid(block_dev_desc_t *device,
				  const disk_partition_t *partition,
				  const char *name, u64 expected_start,
				  u64 expected_blocks, int allow_larger)
{
	if (!device->lba || !device->blksz || !device->block_read) {
		printf("WXR USB: device for partition '%s' is invalid\n", name);
		return -EINVAL;
	}

	if (!partition->size || partition->start >= device->lba ||
	    partition->size > device->lba - partition->start ||
	    partition->blksz != device->blksz) {
		printf("WXR USB: partition '%s' exceeds device bounds\n", name);
		return -EINVAL;
	}

	if (partition->start != expected_start ||
	    (allow_larger ? partition->size < expected_blocks :
			    partition->size != expected_blocks)) {
		printf("WXR USB: partition '%s' has unexpected geometry\n", name);
		return -EINVAL;
	}

	if ((u64)partition->size > ~(u64)0 / partition->blksz) {
		printf("WXR USB: partition '%s' byte size overflows\n", name);
		return -EOVERFLOW;
	}

	return 0;
}

static int wxr_find_partition(block_dev_desc_t *device, const char *name,
			      disk_partition_t *partition, u64 start,
			      u64 blocks, int allow_larger)
{
	if (get_partition_info_efi_by_name(device, name, partition))
		return -ENOENT;

	return wxr_partition_is_valid(device, partition, name, start, blocks,
				      allow_larger);
}

static int wxr_usb_open(enum wxr_usb_role role,
			struct wxr_usb_layout *layout)
{
	struct wxr_usb_layout candidate;
	block_dev_desc_t *device;
	int storage_count = 0;
	int match_count = 0;
	int devnum;
	extern char usb_started;

	if (usb_started && usb_stop()) {
		puts("WXR USB: failed to stop the previous USB scan\n");
		return -EIO;
	}

	wxr_set_status(WXR_STATUS_WAITING);
	if (usb_init()) {
		puts("WXR USB: controller initialization failed\n");
		return -EIO;
	}

	for (devnum = 0; devnum < USB_MAX_STOR_DEV; devnum++) {
		device = usb_stor_get_dev(devnum);
		if (!device || device->type == DEV_TYPE_UNKNOWN)
			continue;

		storage_count++;
		memset(&candidate, 0, sizeof(candidate));
		candidate.devnum = devnum;
		candidate.device = device;

		if (role == WXR_USB_RECOVERY) {
			if (wxr_find_partition(device,
					       WXR_USB_RECOVERY_PARTITION,
					       &candidate.recovery,
					       WXR_USB_RECOVERY_START,
					       WXR_USB_RECOVERY_BLOCKS, 0))
				continue;
		} else {
			if (wxr_find_partition(device,
					       WXR_USB_PRODUCTION_PARTITION,
					       &candidate.production,
					       WXR_USB_PRODUCTION_START,
					       WXR_USB_PRODUCTION_BLOCKS, 0) ||
			    wxr_find_partition(device,
					       WXR_USB_ROOTFS_PARTITION,
					       &candidate.rootfs,
					       WXR_USB_ROOTFS_START,
					       WXR_USB_ROOTFS_BLOCKS, 0) ||
			    wxr_find_partition(device,
					       WXR_USB_OVERLAY_PARTITION,
					       &candidate.overlay,
					       WXR_USB_OVERLAY_START,
					       WXR_USB_OVERLAY_MIN_BLOCKS, 1))
				continue;
		}

		if (match_count) {
			puts("WXR USB: multiple devices match the selected layout\n");
			return -EINVAL;
		}

		*layout = candidate;
		match_count++;
	}

	if (!storage_count) {
		puts("WXR USB: no USB storage device found\n");
		return -ENODEV;
	}

	if (!match_count) {
		puts("WXR USB: no device matches the selected partition layout\n");
		return -ENOENT;
	}

	return 0;
}

static int wxr_usb_read_fit(const struct wxr_usb_layout *layout,
			    const disk_partition_t *partition,
			    size_t *fit_length)
{
	void *fit = (void *)WXR_INPUT_ADDRESS;
	u64 partition_length = wxr_partition_bytes(partition);
	lbaint_t blocks;
	u32 total_size;
	ulong read_blocks;

	if (layout->device->blksz < sizeof(struct fdt_header)) {
		puts("WXR USB: device block is shorter than a FIT header\n");
		return -EINVAL;
	}

	read_blocks = layout->device->block_read(layout->devnum,
						 partition->start, 1, fit);
	if (read_blocks != 1) {
		puts("WXR USB: failed to read the FIT header\n");
		return -EIO;
	}

	if (fdt_check_header(fit)) {
		puts("WXR USB: partition does not begin with a FIT\n");
		return -EINVAL;
	}

	total_size = fdt_totalsize(fit);
	if (total_size < sizeof(struct fdt_header) ||
	    total_size > WXR_INPUT_MAX_SIZE || total_size > partition_length) {
		puts("WXR USB: FIT length is outside the input or partition boundary\n");
		return -EFBIG;
	}

	blocks = DIV_ROUND_UP(total_size, partition->blksz);
	if (blocks > partition->size) {
		puts("WXR USB: rounded FIT read exceeds its partition\n");
		return -EFBIG;
	}

	wxr_set_status(WXR_STATUS_RECEIVING);
	read_blocks = layout->device->block_read(layout->devnum,
						 partition->start, blocks, fit);
	if (read_blocks != blocks) {
		puts("WXR USB: FIT read was incomplete\n");
		return -EIO;
	}
#ifdef CONFIG_SHOW_ACTIVITY
	show_activity(0);
#endif

	if (fdt_check_header(fit) || fdt_totalsize(fit) != total_size) {
		puts("WXR USB: FIT header changed during the bounded read\n");
		return -EINVAL;
	}

	*fit_length = total_size;
	return 0;
}

static int wxr_fit_has_hash(const void *fit, int image_node)
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

static int wxr_fit_contains(const struct wxr_member *image,
			    const void *data, size_t length)
{
	ulong image_address = (ulong)image->data;
	ulong data_address = (ulong)data;

	if (data_address < image_address)
		return 0;
	data_address -= image_address;
	return data_address <= image->length &&
		length <= image->length - data_address;
}

static int wxr_validate_usb_production(const struct wxr_member *image,
				       struct wxr_rootfs_contract *rootfs)
{
	const void *fit;
	const void *contract_data;
	const void *kernel_data;
	const void *fdt_data;
	const fdt32_t *version;
	const fdt64_t *rootfs_bytes;
	const u8 *rootfs_sha256;
	const char *role;
	size_t contract_size;
	size_t kernel_size;
	size_t fdt_size;
	ulong kernel_load;
	ulong kernel_entry;
	fdt64_t encoded_bytes;
	int config_node;
	int kernel_node;
	int fdt_node;
	int contract_node;
	int property_length;

	if (!image || !image->data || !rootfs ||
	    image->length < sizeof(struct fdt_header) ||
	    image->length > WXR_INPUT_MAX_SIZE)
		return -EINVAL;

	fit = image->data;
	if (fdt_check_header(fit) || fdt_totalsize(fit) != image->length ||
	    !fit_check_format(fit)) {
		puts("WXR USB production: malformed FIT\n");
		return -EINVAL;
	}

	config_node = fit_conf_get_node(fit, WXR_FIT_CONFIG);
	kernel_node = config_node < 0 ? config_node :
		fit_conf_get_prop_node(fit, config_node, FIT_KERNEL_PROP);
	fdt_node = config_node < 0 ? config_node :
		fit_conf_get_prop_node(fit, config_node, FIT_FDT_PROP);
	contract_node = fit_image_get_node(fit, WXR_USB_CONTRACT_IMAGE);
	if (config_node < 0 || kernel_node < 0 || fdt_node < 0 ||
	    contract_node < 0) {
		puts("WXR USB production: required FIT node is missing\n");
		return -ENOENT;
	}

	if (!fit_image_check_type(fit, kernel_node, IH_TYPE_KERNEL) ||
	    !fit_image_check_arch(fit, kernel_node, IH_ARCH_ARM64) ||
	    !fit_image_check_os(fit, kernel_node, IH_OS_LINUX) ||
	    !fit_image_check_comp(fit, kernel_node, IH_COMP_NONE) ||
	    fit_image_get_load(fit, kernel_node, &kernel_load) ||
	    fit_image_get_entry(fit, kernel_node, &kernel_entry) ||
	    fit_image_get_data(fit, kernel_node, &kernel_data, &kernel_size) ||
	    kernel_load != WXR_KERNEL_ADDRESS ||
	    kernel_entry != WXR_KERNEL_ADDRESS || !kernel_size ||
	    kernel_size > WXR_KERNEL_MAX_SIZE ||
	    !wxr_fit_contains(image, kernel_data, kernel_size)) {
		puts("WXR USB production: kernel contract is invalid\n");
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, fdt_node, IH_TYPE_FLATDT) ||
	    !fit_image_check_arch(fit, fdt_node, IH_ARCH_ARM64) ||
	    !fit_image_check_comp(fit, fdt_node, IH_COMP_NONE) ||
	    fit_image_get_data(fit, fdt_node, &fdt_data, &fdt_size) ||
	    !fdt_size || !wxr_fit_contains(image, fdt_data, fdt_size) ||
	    fdt_check_header(fdt_data) || fdt_totalsize(fdt_data) != fdt_size ||
	    fdt_node_check_compatible(fdt_data, 0, WXR_BOARD_COMPATIBLE)) {
		puts("WXR USB production: device-tree contract is invalid\n");
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, contract_node, IH_TYPE_FIRMWARE) ||
	    !fit_image_check_arch(fit, contract_node, IH_ARCH_ARM64) ||
	    !fit_image_check_comp(fit, contract_node, IH_COMP_NONE) ||
	    !wxr_fit_has_hash(fit, kernel_node) ||
	    !wxr_fit_has_hash(fit, fdt_node) ||
	    !wxr_fit_has_hash(fit, contract_node) ||
	    !fit_image_verify(fit, kernel_node) ||
	    !fit_image_verify(fit, fdt_node) ||
	    !fit_image_verify(fit, contract_node)) {
		puts("WXR USB production: FIT component hash failed\n");
		return -EIO;
	}

	if (fit_image_get_data(fit, contract_node, &contract_data,
			       &contract_size) ||
	    !wxr_fit_contains(image, contract_data, contract_size) ||
	    fdt_check_header(contract_data) ||
	    fdt_totalsize(contract_data) != contract_size ||
	    fdt_node_check_compatible(contract_data, 0,
				      WXR_USB_CONTRACT_COMPATIBLE)) {
		puts("WXR USB production: boot contract data is invalid\n");
		return -EINVAL;
	}

	version = fdt_getprop(contract_data, 0,
			      "openwrt,boot-contract-version",
			      &property_length);
	if (!version || property_length != sizeof(*version) ||
	    fdt32_to_cpu(*version) != WXR_USB_CONTRACT_VERSION) {
		puts("WXR USB production: boot contract version is invalid\n");
		return -EINVAL;
	}

	role = fdt_getprop(contract_data, 0, "openwrt,image-role",
			   &property_length);
	if (!role || property_length != sizeof("usb-production") ||
	    memcmp(role, "usb-production", sizeof("usb-production"))) {
		puts("WXR USB production: image role is invalid\n");
		return -EINVAL;
	}

	rootfs_bytes = fdt_getprop(contract_data, 0,
				   "openwrt,rootfs-bytes", &property_length);
	if (!rootfs_bytes || property_length != sizeof(*rootfs_bytes)) {
		puts("WXR USB production: rootfs byte contract is invalid\n");
		return -EINVAL;
	}
	memcpy(&encoded_bytes, rootfs_bytes, sizeof(encoded_bytes));
	rootfs->bytes_used = fdt64_to_cpu(encoded_bytes);
	if (!rootfs->bytes_used) {
		puts("WXR USB production: rootfs byte contract is zero\n");
		return -EINVAL;
	}

	rootfs_sha256 = fdt_getprop(contract_data, 0,
				    "openwrt,rootfs-sha256",
				    &property_length);
	if (!rootfs_sha256 || property_length != SHA256_SUM_LEN) {
		puts("WXR USB production: rootfs SHA-256 contract is invalid\n");
		return -EINVAL;
	}
	memcpy(rootfs->sha256, rootfs_sha256, SHA256_SUM_LEN);

	return 0;
}

static int wxr_hash_usb_partition(const struct wxr_usb_layout *layout,
				  const disk_partition_t *partition,
				  size_t length,
				  u8 digest[SHA256_SUM_LEN])
{
	sha256_context context;
	u8 *buffer;
	size_t remaining = length;
	lbaint_t current = partition->start;
	lbaint_t blocks_per_read;

	if (!length || length > wxr_partition_bytes(partition))
		return -EFBIG;

	blocks_per_read = WXR_USB_IO_BUFFER_SIZE / partition->blksz;
	if (!blocks_per_read)
		return -EINVAL;

	buffer = memalign(ARCH_DMA_MINALIGN, WXR_USB_IO_BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;

	sha256_starts(&context);
	while (remaining) {
		lbaint_t blocks = min((lbaint_t)DIV_ROUND_UP(remaining,
							       partition->blksz),
				      blocks_per_read);
		size_t bytes = min(remaining,
				   (size_t)(blocks * partition->blksz));

		if (layout->device->block_read(layout->devnum, current, blocks,
						       buffer) != blocks) {
			free(buffer);
			return -EIO;
		}
		sha256_update(&context, buffer, bytes);
		remaining -= bytes;
		current += blocks;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	sha256_finish(&context, digest);
	free(buffer);
	return 0;
}

static int wxr_verify_usb_rootfs(const struct wxr_usb_layout *layout,
				 const struct wxr_rootfs_contract *rootfs)
{
	u8 calculated[SHA256_SUM_LEN];
	u8 *header;
	struct wxr_member root;
	u64 bytes_used;
	int ret;

	if (rootfs->bytes_used > wxr_partition_bytes(&layout->rootfs) ||
	    rootfs->bytes_used > SIZE_MAX)
		return -EFBIG;

	header = memalign(ARCH_DMA_MINALIGN, layout->rootfs.blksz);
	if (!header)
		return -ENOMEM;
	if (layout->device->block_read(layout->devnum, layout->rootfs.start,
				       1, header) != 1) {
		free(header);
		return -EIO;
	}

	root.data = header;
	root.length = wxr_partition_bytes(&layout->rootfs);
	ret = wxr_validate_squashfs(&root, &bytes_used);
	free(header);
	if (ret || bytes_used != rootfs->bytes_used) {
		puts("WXR USB production: SquashFS length differs from FIT contract\n");
		return -EINVAL;
	}

	ret = wxr_hash_usb_partition(layout, &layout->rootfs,
				     rootfs->bytes_used, calculated);
	if (ret)
		return ret;

	if (memcmp(calculated, rootfs->sha256, SHA256_SUM_LEN)) {
		puts("WXR USB production: rootfs SHA-256 mismatch\n");
		return -EIO;
	}

	return 0;
}

static int wxr_boot_usb_production_fit(cmd_tbl_t *cmdtp,
				       const struct wxr_usb_layout *layout)
{
	char previous_bootargs[CONFIG_SYS_CBSIZE];
	char bootargs[CONFIG_SYS_CBSIZE];
	char *bootm_argv[] = {
		"bootm",
		"0x50000000#config@hk01",
		NULL,
	};
	const char *current = getenv("bootargs");
	int had_bootargs = current != NULL;
	int length;
	int ret;

	if (!layout->rootfs.uuid[0]) {
		puts("WXR USB production: rootfs PARTUUID is missing\n");
		return -EINVAL;
	}

	if (had_bootargs) {
		if (strlen(current) >= sizeof(previous_bootargs))
			return -E2BIG;
		strlcpy(previous_bootargs, current, sizeof(previous_bootargs));
	}

	length = snprintf(bootargs, sizeof(bootargs),
			  CONFIG_BOOTARGS
			  " root=PARTUUID=%s"
			  " rootfstype=squashfs"
			  " rootwait"
			  " fstools_partname_fallback_scan=1"
			  " fstools_overlay_fstype=ext4",
			  layout->rootfs.uuid);
	if (length < 0 || length >= (int)sizeof(bootargs))
		return -E2BIG;

	if (setenv("bootargs", bootargs))
		return -ENOMEM;

	wxr_set_status(WXR_STATUS_SUCCESS);
	ret = do_bootm(cmdtp, 0, 2, bootm_argv);

	if (had_bootargs)
		setenv("bootargs", previous_bootargs);
	else
		setenv("bootargs", NULL);

	printf("WXR USB production: bootm returned %d\n", ret);
	wxr_set_status(WXR_STATUS_FAILURE);
	return -EIO;
}

int wxr_usb_boot_recovery(cmd_tbl_t *cmdtp)
{
	struct wxr_usb_layout layout;
	struct wxr_image image;
	size_t fit_length;
	int ret;

	ret = wxr_usb_open(WXR_USB_RECOVERY, &layout);
	if (ret)
		return ret;
	ret = wxr_usb_read_fit(&layout, &layout.recovery, &fit_length);
	if (ret)
		return ret;
	ret = wxr_set_input(fit_length, &image);
	if (ret)
		return ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	return wxr_boot_recovery(cmdtp, &image);
}

int wxr_usb_boot_production(cmd_tbl_t *cmdtp)
{
	struct wxr_rootfs_contract rootfs;
	struct wxr_usb_layout layout;
	struct wxr_member fit;
	size_t fit_length;
	int ret;

	ret = wxr_usb_open(WXR_USB_PRODUCTION, &layout);
	if (ret)
		return ret;
	ret = wxr_usb_read_fit(&layout, &layout.production, &fit_length);
	if (ret)
		return ret;

	fit.data = (const void *)WXR_INPUT_ADDRESS;
	fit.length = fit_length;
	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_validate_usb_production(&fit, &rootfs);
	if (ret)
		return ret;
	ret = wxr_verify_usb_rootfs(&layout, &rootfs);
	if (ret)
		return ret;

	return wxr_boot_usb_production_fit(cmdtp, &layout);
}

static int wxr_write_usb_member(const struct wxr_usb_layout *layout,
				const disk_partition_t *partition,
				const struct wxr_member *member)
{
	const u8 *source = member->data;
	u8 *buffer;
	size_t remaining = member->length;
	lbaint_t current = partition->start;

	if (!layout->device->block_write || !member->length ||
	    member->length > wxr_partition_bytes(partition))
		return -EFBIG;
	if (partition->blksz > WXR_USB_IO_BUFFER_SIZE)
		return -EINVAL;

	buffer = memalign(ARCH_DMA_MINALIGN, WXR_USB_IO_BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;

	while (remaining) {
		size_t bytes = min(remaining, (size_t)WXR_USB_IO_BUFFER_SIZE);
		lbaint_t blocks = DIV_ROUND_UP(bytes, partition->blksz);
		size_t transfer = blocks * partition->blksz;

		memset(buffer, 0, transfer);
		memcpy(buffer, source, bytes);
		if (layout->device->block_write(layout->devnum, current, blocks,
							buffer) != blocks) {
			free(buffer);
			return -EIO;
		}
		source += bytes;
		remaining -= bytes;
		current += blocks;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	free(buffer);
	return 0;
}

static int wxr_verify_usb_member(const struct wxr_usb_layout *layout,
				 const disk_partition_t *partition,
				 const struct wxr_member *member,
				 const u8 expected[SHA256_SUM_LEN])
{
	u8 actual[SHA256_SUM_LEN];
	int ret;

	ret = wxr_hash_usb_partition(layout, partition, member->length, actual);
	if (ret)
		return ret;
	return memcmp(actual, expected, SHA256_SUM_LEN) ? -EIO : 0;
}

static int wxr_clear_usb_overlay(const struct wxr_usb_layout *layout)
{
	u8 *buffer;
	size_t remaining = WXR_USB_OVERLAY_CLEAR_SIZE;
	lbaint_t current = layout->overlay.start;
	lbaint_t blocks_per_io;

	if (!layout->device->block_write ||
	    WXR_USB_OVERLAY_CLEAR_SIZE > wxr_partition_bytes(&layout->overlay))
		return -EFBIG;

	blocks_per_io = WXR_USB_IO_BUFFER_SIZE / layout->overlay.blksz;
	if (!blocks_per_io)
		return -EINVAL;

	buffer = memalign(ARCH_DMA_MINALIGN, WXR_USB_IO_BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;
	memset(buffer, 0, WXR_USB_IO_BUFFER_SIZE);

	while (remaining) {
		lbaint_t blocks = min((lbaint_t)DIV_ROUND_UP(remaining,
							       layout->overlay.blksz),
				      blocks_per_io);
		size_t bytes = blocks * layout->overlay.blksz;
		size_t index;

		if (layout->device->block_write(layout->devnum, current, blocks,
							buffer) != blocks ||
		    layout->device->block_read(layout->devnum, current, blocks,
						       buffer) != blocks) {
			free(buffer);
			return -EIO;
		}
		for (index = 0; index < bytes; index++)
			if (buffer[index]) {
				free(buffer);
				return -EIO;
			}

		memset(buffer, 0, WXR_USB_IO_BUFFER_SIZE);
		remaining -= min(remaining, bytes);
		current += blocks;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	free(buffer);
	return 0;
}

int wxr_usb_write_recovery(const struct wxr_image *image)
{
	struct wxr_usb_layout layout;
	struct wxr_image readback;
	struct wxr_member member;
	u8 expected[SHA256_SUM_LEN];
	size_t readback_length;
	int ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_validate_recovery(image);
	if (ret)
		return ret;
	ret = wxr_usb_open(WXR_USB_RECOVERY, &layout);
	if (ret)
		return ret;

	member.data = (const void *)image->address;
	member.length = image->length;
	wxr_sha256(member.data, member.length, expected);
	wxr_set_status(WXR_STATUS_WRITING);
	ret = wxr_write_usb_member(&layout, &layout.recovery, &member);
	if (ret)
		return ret;
	ret = wxr_verify_usb_member(&layout, &layout.recovery, &member,
				    expected);
	if (ret) {
		puts("WXR USB recovery write: readback SHA-256 mismatch\n");
		return ret;
	}

	ret = wxr_usb_read_fit(&layout, &layout.recovery, &readback_length);
	if (ret)
		return ret;
	ret = wxr_set_input(readback_length, &readback);
	if (ret)
		return ret;
	ret = wxr_validate_recovery(&readback);
	if (ret)
		return ret;

	wxr_set_status(WXR_STATUS_SUCCESS);
	return 0;
}

int wxr_usb_write_production(const struct wxr_image *image)
{
	struct wxr_rootfs_contract rootfs_contract;
	struct wxr_sysupgrade archive;
	struct wxr_usb_layout layout;
	struct wxr_member readback_fit;
	u8 kernel_sha256[SHA256_SUM_LEN];
	u8 root_sha256[SHA256_SUM_LEN];
	u8 contract_sha256[SHA256_SUM_LEN];
	u64 root_bytes_used;
	size_t readback_length;
	int ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_parse_sysupgrade(image, WXR_SYSUPGRADE_USB, &archive);
	if (ret)
		return ret;
	ret = wxr_validate_usb_production(&archive.kernel, &rootfs_contract);
	if (ret)
		return ret;
	ret = wxr_validate_squashfs(&archive.root, &root_bytes_used);
	if (ret)
		return ret;
	if (root_bytes_used != rootfs_contract.bytes_used) {
		puts("WXR USB production write: rootfs length contract mismatch\n");
		return -EINVAL;
	}
	wxr_sha256(archive.root.data, root_bytes_used, contract_sha256);
	if (memcmp(contract_sha256, rootfs_contract.sha256, SHA256_SUM_LEN)) {
		puts("WXR USB production write: rootfs contract hash mismatch\n");
		return -EIO;
	}

	ret = wxr_usb_open(WXR_USB_PRODUCTION, &layout);
	if (ret)
		return ret;
	ret = wxr_find_partition(layout.device, WXR_USB_RECOVERY_PARTITION,
				 &layout.recovery, WXR_USB_RECOVERY_START,
				 WXR_USB_RECOVERY_BLOCKS, 0);
	if (ret) {
		puts("WXR USB production write: recovery partition is invalid\n");
		return ret;
	}
	if (archive.kernel.length > wxr_partition_bytes(&layout.production) ||
	    archive.root.length > wxr_partition_bytes(&layout.rootfs))
		return -EFBIG;

	wxr_sha256(archive.kernel.data, archive.kernel.length, kernel_sha256);
	wxr_sha256(archive.root.data, archive.root.length, root_sha256);
	wxr_set_status(WXR_STATUS_WRITING);

	ret = wxr_write_usb_member(&layout, &layout.rootfs, &archive.root);
	if (ret)
		return ret;
	ret = wxr_verify_usb_member(&layout, &layout.rootfs, &archive.root,
				    root_sha256);
	if (ret) {
		puts("WXR USB production write: rootfs readback mismatch\n");
		return ret;
	}
	ret = wxr_clear_usb_overlay(&layout);
	if (ret) {
		puts("WXR USB production write: overlay reset verification failed\n");
		return ret;
	}
	ret = wxr_write_usb_member(&layout, &layout.production,
				   &archive.kernel);
	if (ret)
		return ret;
	ret = wxr_verify_usb_member(&layout, &layout.production,
				    &archive.kernel, kernel_sha256);
	if (ret) {
		puts("WXR USB production write: FIT readback mismatch\n");
		return ret;
	}

	ret = wxr_usb_read_fit(&layout, &layout.production, &readback_length);
	if (ret)
		return ret;
	readback_fit.data = (const void *)WXR_INPUT_ADDRESS;
	readback_fit.length = readback_length;
	ret = wxr_validate_usb_production(&readback_fit, &rootfs_contract);
	if (ret)
		return ret;

	wxr_set_status(WXR_STATUS_SUCCESS);
	return 0;
}

static int do_wxr_usbboot(cmd_tbl_t *cmdtp, int flag, int argc,
			  char *const argv[])
{
	int ret;

	(void)flag;
	if (argc != 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "recovery"))
		ret = wxr_usb_boot_recovery(cmdtp);
	else if (!strcmp(argv[1], "production"))
		ret = wxr_usb_boot_production(cmdtp);
	else
		return CMD_RET_USAGE;

	if (ret)
		wxr_set_status(WXR_STATUS_FAILURE);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	wxr_usbboot, 2, 0, do_wxr_usbboot,
	"boot WXR-5950AX12 from raw USB GPT partitions",
	"recovery  - boot the raw wxr_recovery FIT\n"
	"wxr_usbboot production - boot wxr_production with USB rootfs"
);
