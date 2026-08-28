/*
 * Buffalo WXR-5950AX12 raw USB partition boot command.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <hash.h>
#include <image.h>
#include <malloc.h>
#include <part.h>
#include <usb.h>
#include <asm/cache.h>
#include <u-boot/sha256.h>

#define WXR_USB_FIT_ADDRESS		0x44000000UL
#define WXR_USB_FIT_MAX_SIZE		(64UL << 20)
#define WXR_USB_KERNEL_ADDRESS		0x41000000UL
#define WXR_USB_HASH_BUFFER_SIZE	(64UL << 10)

#define WXR_USB_RECOVERY_PARTITION	"wxr_recovery"
#define WXR_USB_PRODUCTION_PARTITION	"wxr_production"
#define WXR_USB_ROOTFS_PARTITION	"wxr_rootfs"
#define WXR_USB_OVERLAY_PARTITION	"rootfs_data"

#define WXR_USB_FIT_CONFIG		"config@hk01"
#define WXR_USB_CONTRACT_IMAGE		"contract@1"
#define WXR_USB_CONTRACT_COMPATIBLE	\
	"openwrt,wxr-5950ax12-boot-contract"
#define WXR_USB_CONTRACT_VERSION	1

#define WXR_SQUASHFS_HEADER_SIZE	48
#define WXR_SQUASHFS_BYTES_USED_OFFSET	40

enum wxr_usb_role {
	WXR_USB_RECOVERY,
	WXR_USB_PRODUCTION,
};

struct wxr_usb_layout {
	int devnum;
	block_dev_desc_t *device;
	disk_partition_t fit;
	disk_partition_t rootfs;
	disk_partition_t overlay;
};

struct wxr_rootfs_contract {
	u64 bytes_used;
	u8 sha256[SHA256_SUM_LEN];
};

static int wxr_partition_is_valid(block_dev_desc_t *device,
				  const disk_partition_t *partition,
				  const char *name)
{
	if (!device->lba || !device->blksz || !device->block_read) {
		printf("WXR USB boot: device for partition '%s' is invalid\n",
		       name);
		return -EINVAL;
	}

	if (!partition->size) {
		printf("WXR USB boot: partition '%s' is empty\n", name);
		return -EINVAL;
	}

	if (partition->start >= device->lba ||
	    partition->size > device->lba - partition->start) {
		printf("WXR USB boot: partition '%s' exceeds device bounds\n",
		       name);
		return -EINVAL;
	}

	if (partition->blksz != device->blksz) {
		printf("WXR USB boot: partition '%s' has an invalid block size\n",
		       name);
		return -EINVAL;
	}

	if ((u64)partition->size > ~(u64)0 / partition->blksz) {
		printf("WXR USB boot: partition '%s' byte size overflows\n",
		       name);
		return -EINVAL;
	}

	return 0;
}

static int wxr_find_usb_device(enum wxr_usb_role role,
			       struct wxr_usb_layout *layout)
{
	struct wxr_usb_layout candidate;
	block_dev_desc_t *device;
	int storage_count = 0;
	int match_count = 0;
	int devnum;

	for (devnum = 0; devnum < USB_MAX_STOR_DEV; devnum++) {
		device = usb_stor_get_dev(devnum);
		if (!device || device->type == DEV_TYPE_UNKNOWN)
			continue;

		storage_count++;
		memset(&candidate, 0, sizeof(candidate));
		candidate.devnum = devnum;
		candidate.device = device;

		if (role == WXR_USB_RECOVERY) {
			if (get_partition_info_efi_by_name(device,
					WXR_USB_RECOVERY_PARTITION,
					&candidate.fit))
				continue;

			if (wxr_partition_is_valid(device, &candidate.fit,
						   WXR_USB_RECOVERY_PARTITION))
				continue;
		} else {
			if (get_partition_info_efi_by_name(device,
					WXR_USB_PRODUCTION_PARTITION,
					&candidate.fit) ||
			    get_partition_info_efi_by_name(device,
					WXR_USB_ROOTFS_PARTITION,
					&candidate.rootfs) ||
			    get_partition_info_efi_by_name(device,
					WXR_USB_OVERLAY_PARTITION,
					&candidate.overlay))
				continue;

			if (wxr_partition_is_valid(device, &candidate.fit,
						   WXR_USB_PRODUCTION_PARTITION) ||
			    wxr_partition_is_valid(device, &candidate.rootfs,
						   WXR_USB_ROOTFS_PARTITION) ||
			    wxr_partition_is_valid(device, &candidate.overlay,
						   WXR_USB_OVERLAY_PARTITION))
				continue;
		}

		if (match_count) {
			puts("WXR USB boot: multiple matching USB boot devices\n");
			return -EINVAL;
		}

		*layout = candidate;
		match_count++;
	}

	if (!storage_count) {
		puts("WXR USB boot: no USB storage device found\n");
		return -ENODEV;
	}

	if (!match_count) {
		puts("WXR USB boot: no device matches the requested layout\n");
		return -ENOENT;
	}

	return 0;
}

static int wxr_load_fit(const struct wxr_usb_layout *layout,
			unsigned long *fit_size)
{
	void *fit = (void *)WXR_USB_FIT_ADDRESS;
	u64 partition_bytes;
	lbaint_t block_count;
	u32 total_size;
	ulong blocks_read;

	if (layout->device->blksz < sizeof(struct fdt_header)) {
		puts("WXR USB boot: device block is shorter than a FIT header\n");
		return -EINVAL;
	}

	blocks_read = layout->device->block_read(layout->devnum,
						layout->fit.start, 1, fit);
	if (blocks_read != 1) {
		puts("WXR USB boot: failed to read FIT header\n");
		return -EIO;
	}

	if (fdt_check_header(fit)) {
		puts("WXR USB boot: invalid FIT header\n");
		return -EINVAL;
	}

	total_size = fdt_totalsize(fit);
	partition_bytes = (u64)layout->fit.size * layout->fit.blksz;
	if (total_size < sizeof(struct fdt_header) ||
	    total_size > WXR_USB_FIT_MAX_SIZE ||
	    total_size > partition_bytes) {
		puts("WXR USB boot: FIT size is outside allowed bounds\n");
		return -EFBIG;
	}

	block_count = DIV_ROUND_UP(total_size, layout->fit.blksz);
	if (block_count > layout->fit.size) {
		puts("WXR USB boot: rounded FIT read exceeds partition\n");
		return -EFBIG;
	}

	blocks_read = layout->device->block_read(layout->devnum,
						layout->fit.start,
						block_count, fit);
	if (blocks_read != block_count) {
		puts("WXR USB boot: FIT read was incomplete\n");
		return -EIO;
	}

	if (fdt_check_header(fit) || fdt_totalsize(fit) != total_size) {
		puts("WXR USB boot: FIT changed during bounded read\n");
		return -EINVAL;
	}

	*fit_size = total_size;
	return 0;
}

static int wxr_validate_fit(enum wxr_usb_role role, unsigned long fit_size,
			    struct wxr_rootfs_contract *rootfs)
{
	const void *fit = (const void *)WXR_USB_FIT_ADDRESS;
	const void *contract_data;
	const void *kernel_data;
	const fdt32_t *version;
	const fdt64_t *rootfs_bytes;
	const u8 *rootfs_sha256;
	const char *hash_algorithm;
	const char *image_role;
	const char *expected_role;
	size_t contract_size;
	size_t kernel_size;
	ulong kernel_load;
	ulong kernel_entry;
	fdt64_t encoded_bytes;
	int contract_node;
	int config_node;
	int kernel_node;
	int fdt_node;
	int hash_node;
	int property_length;

	if (fit_size != fdt_totalsize(fit) || !fit_check_format(fit)) {
		puts("WXR USB boot: malformed FIT\n");
		return -EINVAL;
	}

	config_node = fit_conf_get_node(fit, WXR_USB_FIT_CONFIG);
	if (config_node < 0) {
		puts("WXR USB boot: config@hk01 is missing\n");
		return -EINVAL;
	}

	kernel_node = fit_conf_get_prop_node(fit, config_node,
					     FIT_KERNEL_PROP);
	fdt_node = fit_conf_get_prop_node(fit, config_node, FIT_FDT_PROP);
	contract_node = fit_image_get_node(fit, WXR_USB_CONTRACT_IMAGE);
	if (kernel_node < 0 || fdt_node < 0 || contract_node < 0) {
		puts("WXR USB boot: required FIT node is missing\n");
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, kernel_node, IH_TYPE_KERNEL) ||
	    !fit_image_check_arch(fit, kernel_node, IH_ARCH_ARM64) ||
	    !fit_image_check_os(fit, kernel_node, IH_OS_LINUX) ||
	    !fit_image_check_comp(fit, kernel_node, IH_COMP_NONE) ||
	    fit_image_get_load(fit, kernel_node, &kernel_load) ||
	    fit_image_get_entry(fit, kernel_node, &kernel_entry) ||
	    fit_image_get_data(fit, kernel_node, &kernel_data, &kernel_size)) {
		puts("WXR USB boot: kernel contract is invalid\n");
		return -EINVAL;
	}

	if (kernel_load != WXR_USB_KERNEL_ADDRESS ||
	    kernel_entry != WXR_USB_KERNEL_ADDRESS || !kernel_size ||
	    kernel_size > WXR_USB_FIT_ADDRESS - kernel_load) {
		puts("WXR USB boot: kernel load range is invalid\n");
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, fdt_node, IH_TYPE_FLATDT) ||
	    !fit_image_check_arch(fit, fdt_node, IH_ARCH_ARM64) ||
	    !fit_image_check_comp(fit, fdt_node, IH_COMP_NONE)) {
		puts("WXR USB boot: device-tree contract is invalid\n");
		return -EINVAL;
	}

	if (!fit_image_check_type(fit, contract_node, IH_TYPE_FIRMWARE) ||
	    !fit_image_check_arch(fit, contract_node, IH_ARCH_ARM64) ||
	    !fit_image_check_comp(fit, contract_node, IH_COMP_NONE)) {
		puts("WXR USB boot: contract image description is invalid\n");
		return -EINVAL;
	}

	hash_node = fdt_subnode_offset(fit, contract_node, "hash@1");
	hash_algorithm = hash_node < 0 ? NULL :
		fdt_getprop(fit, hash_node, FIT_ALGO_PROP, &property_length);
	if (!hash_algorithm || property_length != sizeof("sha256") ||
	    memcmp(hash_algorithm, "sha256", sizeof("sha256"))) {
		puts("WXR USB boot: contract SHA-256 node is missing\n");
		return -EINVAL;
	}

	if (!fit_image_verify(fit, contract_node)) {
		puts("WXR USB boot: contract SHA-256 verification failed\n");
		return -EIO;
	}

	if (fit_image_get_data(fit, contract_node, &contract_data,
			       &contract_size) ||
	    fdt_check_header(contract_data) ||
	    fdt_totalsize(contract_data) != contract_size) {
		puts("WXR USB boot: contract data is invalid\n");
		return -EINVAL;
	}

	if (fdt_node_check_compatible(contract_data, 0,
				      WXR_USB_CONTRACT_COMPATIBLE)) {
		puts("WXR USB boot: contract compatible is invalid\n");
		return -EINVAL;
	}

	version = fdt_getprop(contract_data, 0,
			      "openwrt,boot-contract-version",
			      &property_length);
	if (!version || property_length != sizeof(*version) ||
	    fdt32_to_cpu(*version) != WXR_USB_CONTRACT_VERSION) {
		puts("WXR USB boot: contract version is invalid\n");
		return -EINVAL;
	}

	expected_role = role == WXR_USB_RECOVERY ?
			"recovery" : "usb-production";
	image_role = fdt_getprop(contract_data, 0, "openwrt,image-role",
				 &property_length);
	if (!image_role ||
	    property_length != (int)strlen(expected_role) + 1 ||
	    memcmp(image_role, expected_role, property_length)) {
		printf("WXR USB boot: image role is not '%s'\n", expected_role);
		return -EINVAL;
	}

	if (role == WXR_USB_RECOVERY)
		return 0;

	rootfs_bytes = fdt_getprop(contract_data, 0,
				   "openwrt,rootfs-bytes",
				   &property_length);
	if (!rootfs_bytes || property_length != sizeof(*rootfs_bytes)) {
		puts("WXR USB boot: rootfs byte contract is invalid\n");
		return -EINVAL;
	}
	memcpy(&encoded_bytes, rootfs_bytes, sizeof(encoded_bytes));
	rootfs->bytes_used = fdt64_to_cpu(encoded_bytes);

	rootfs_sha256 = fdt_getprop(contract_data, 0,
				    "openwrt,rootfs-sha256",
				    &property_length);
	if (!rootfs_sha256 || property_length != SHA256_SUM_LEN) {
		puts("WXR USB boot: rootfs SHA-256 contract is invalid\n");
		return -EINVAL;
	}
	memcpy(rootfs->sha256, rootfs_sha256, sizeof(rootfs->sha256));

	return 0;
}

static int wxr_verify_rootfs(const struct wxr_usb_layout *layout,
			     const struct wxr_rootfs_contract *rootfs)
{
	sha256_context sha;
	u8 calculated[SHA256_SUM_LEN];
	u8 *buffer;
	u64 partition_bytes;
	u64 remaining;
	u64 bytes_to_hash;
	u64 read_capacity;
	u64 squashfs_bytes;
	lbaint_t blocks_per_read;
	lbaint_t block_count;
	lbaint_t current_lba;
	lbaint_t offset;
	ulong blocks_read;
	u64 encoded_bytes;
	int first_read = 1;
	int ret = -EIO;

	partition_bytes = (u64)layout->rootfs.size * layout->rootfs.blksz;
	if (rootfs->bytes_used < WXR_SQUASHFS_HEADER_SIZE ||
	    rootfs->bytes_used > partition_bytes) {
		puts("WXR USB boot: rootfs length exceeds partition\n");
		return -EINVAL;
	}

	blocks_per_read = WXR_USB_HASH_BUFFER_SIZE / layout->rootfs.blksz;
	if (!blocks_per_read) {
		puts("WXR USB boot: device block exceeds rootfs hash buffer\n");
		return -EINVAL;
	}

	buffer = memalign(ARCH_DMA_MINALIGN, WXR_USB_HASH_BUFFER_SIZE);
	if (!buffer) {
		puts("WXR USB boot: cannot allocate rootfs hash buffer\n");
		return -ENOMEM;
	}

	sha256_starts(&sha);
	remaining = rootfs->bytes_used;
	current_lba = layout->rootfs.start;

	while (remaining) {
		block_count = blocks_per_read;
		read_capacity = (u64)block_count * layout->rootfs.blksz;
		if (remaining < read_capacity)
			block_count = DIV_ROUND_UP(remaining,
						   layout->rootfs.blksz);

		offset = current_lba - layout->rootfs.start;
		if (offset >= layout->rootfs.size ||
		    block_count > layout->rootfs.size - offset) {
			puts("WXR USB boot: rootfs read exceeds partition\n");
			ret = -EFBIG;
			goto out;
		}

		blocks_read = layout->device->block_read(layout->devnum,
							current_lba,
							block_count,
							buffer);
		if (blocks_read != block_count) {
			puts("WXR USB boot: rootfs read was incomplete\n");
			ret = -EIO;
			goto out;
		}

		if (first_read) {
			if (memcmp(buffer, "hsqs", 4)) {
				puts("WXR USB boot: rootfs is not SquashFS\n");
				ret = -EINVAL;
				goto out;
			}

			memcpy(&encoded_bytes,
			       buffer + WXR_SQUASHFS_BYTES_USED_OFFSET,
			       sizeof(encoded_bytes));
			squashfs_bytes = le64_to_cpu(encoded_bytes);
			if (squashfs_bytes != rootfs->bytes_used) {
				puts("WXR USB boot: SquashFS length differs from FIT contract\n");
				ret = -EINVAL;
				goto out;
			}
			first_read = 0;
		}

		read_capacity = (u64)block_count * layout->rootfs.blksz;
		bytes_to_hash = remaining < read_capacity ?
				remaining : read_capacity;
		sha256_update(&sha, buffer, (u32)bytes_to_hash);
		remaining -= bytes_to_hash;
		current_lba += block_count;
	}

	sha256_finish(&sha, calculated);
	if (memcmp(calculated, rootfs->sha256, sizeof(calculated))) {
		puts("WXR USB boot: rootfs SHA-256 mismatch\n");
		ret = -EIO;
		goto out;
	}

	ret = 0;

out:
	free(buffer);
	return ret;
}

static int wxr_boot_fit(cmd_tbl_t *cmdtp, enum wxr_usb_role role,
			const struct wxr_usb_layout *layout)
{
	char previous_bootargs[CONFIG_SYS_CBSIZE];
	char usb_bootargs[CONFIG_SYS_CBSIZE];
	char *bootm_argv[] = {
		"bootm",
		"0x44000000#config@hk01",
		NULL,
	};
	const char *current;
	int had_bootargs;
	int restore_ret;
	int ret;

	current = getenv("bootargs");
	had_bootargs = current != NULL;
	if (had_bootargs) {
		if (strlen(current) >= sizeof(previous_bootargs)) {
			puts("WXR USB boot: existing bootargs are too long to preserve\n");
			return CMD_RET_FAILURE;
		}
		strlcpy(previous_bootargs, current,
			sizeof(previous_bootargs));
	}

	if (role == WXR_USB_RECOVERY) {
		strlcpy(usb_bootargs, CONFIG_BOOTARGS,
			sizeof(usb_bootargs));
	} else {
		if (!layout->rootfs.uuid[0]) {
			puts("WXR USB boot: rootfs PARTUUID is missing\n");
			return CMD_RET_FAILURE;
		}

		ret = snprintf(usb_bootargs, sizeof(usb_bootargs),
			       CONFIG_BOOTARGS
			       " root=PARTUUID=%s"
			       " rootfstype=squashfs"
			       " rootwait"
			       " fstools_partname_fallback_scan=1"
			       " fstools_overlay_fstype=ext4",
			       layout->rootfs.uuid);
		if (ret < 0 || ret >= (int)sizeof(usb_bootargs)) {
			puts("WXR USB boot: bootargs are too long\n");
			return CMD_RET_FAILURE;
		}
	}

	if (setenv("bootargs", usb_bootargs)) {
		puts("WXR USB boot: failed to set temporary bootargs\n");
		return CMD_RET_FAILURE;
	}

	ret = do_bootm(cmdtp, 0, 2, bootm_argv);

	if (had_bootargs)
		restore_ret = setenv("bootargs", previous_bootargs);
	else
		restore_ret = setenv("bootargs", NULL);
	if (restore_ret)
		puts("WXR USB boot: failed to restore bootargs\n");

	printf("WXR USB boot: bootm returned %d\n", ret);
	return CMD_RET_FAILURE;
}

static int do_wxr_usbboot(cmd_tbl_t *cmdtp, int flag, int argc,
			  char *const argv[])
{
	struct wxr_rootfs_contract rootfs = { 0 };
	struct wxr_usb_layout layout = { 0 };
	enum wxr_usb_role role;
	unsigned long fit_size;
	extern char usb_started;

	(void)flag;

	if (argc != 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "recovery"))
		role = WXR_USB_RECOVERY;
	else if (!strcmp(argv[1], "production"))
		role = WXR_USB_PRODUCTION;
	else
		return CMD_RET_USAGE;

	if (usb_started && usb_stop()) {
		puts("WXR USB boot: failed to stop the previous USB scan\n");
		return CMD_RET_FAILURE;
	}

	if (usb_init()) {
		puts("WXR USB boot: USB initialization failed\n");
		return CMD_RET_FAILURE;
	}

	if (wxr_find_usb_device(role, &layout) ||
	    wxr_load_fit(&layout, &fit_size) ||
	    wxr_validate_fit(role, fit_size, &rootfs))
		return CMD_RET_FAILURE;

	if (role == WXR_USB_PRODUCTION &&
	    wxr_verify_rootfs(&layout, &rootfs))
		return CMD_RET_FAILURE;

	return wxr_boot_fit(cmdtp, role, &layout);
}

U_BOOT_CMD(
	wxr_usbboot, 2, 0, do_wxr_usbboot,
	"boot WXR-5950AX12 from raw USB GPT partitions",
	"recovery  - boot the raw wxr_recovery FIT\n"
	"wxr_usbboot production - boot wxr_production with USB rootfs"
);
