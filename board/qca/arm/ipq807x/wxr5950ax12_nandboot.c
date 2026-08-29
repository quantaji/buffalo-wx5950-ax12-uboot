/*
 * Buffalo WXR-5950AX12 NAND boot and UBI update transactions.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <jffs2/load_kernel.h>
#include <malloc.h>
#include <nand.h>
#include <ubi_uboot.h>
#include <asm/cache.h>
#include <asm/arch-qca-common/qca_common.h>
#include <asm/arch-qca-common/smem.h>
#include <u-boot/sha256.h>
#include <watchdog.h>

#include "wxr5950ax12_boot.h"

#define WXR_NAND_RECOVERY_PARTITION	"rootfs_recover"
#define WXR_NAND_PRODUCTION_PARTITION	"rootfs"
#define WXR_NAND_USER_PARTITION		"user_property"
#define WXR_NAND_KERNEL_VOLUME		"kernel"
#define WXR_NAND_ROOTFS_VOLUME		"rootfs"
#define WXR_NAND_DATA_VOLUME		"rootfs_data"
#define WXR_NAND_COMPAT_ROOTFS_VOLUME	"ubi_rootfs"
#define WXR_NAND_FW_HASH_VOLUME		"fw_hash"
#define WXR_NAND_IO_BUFFER_SIZE		(64UL << 10)

static const u8 wxr_nand_fw_hash_reset[] =
	"00000000000000000000000000000000";

struct wxr_nand_partition {
	nand_info_t *nand;
	loff_t offset;
	loff_t length;
};

static int wxr_resolve_nand_partition(const char *name,
				      struct wxr_nand_partition *partition)
{
	qca_smem_flash_info_t *flash = &qca_smem_flash_info;
	u32 start_blocks;
	u32 size_blocks;
	u64 offset;
	u64 length;

	if (flash->flash_type != SMEM_BOOT_NAND_FLASH &&
	    flash->flash_type != SMEM_BOOT_QSPI_NAND_FLASH) {
		puts("WXR NAND: boot flash is not NAND\n");
		wxr_error_set(WXR_ERROR_TARGET, name, "boot flash type",
			      -ENODEV, 0);
		return -ENODEV;
	}

	if (!flash->flash_block_size ||
	    smem_getpart((char *)name, &start_blocks, &size_blocks) ||
	    !size_blocks) {
		printf("WXR NAND: SMEM partition '%s' is unavailable\n", name);
		wxr_error_set(WXR_ERROR_TARGET, name, "SMEM partition lookup",
			      -ENOENT, 0);
		return -ENOENT;
	}

	offset = (u64)flash->flash_block_size * start_blocks;
	length = (u64)flash->flash_block_size * size_blocks;
	partition->nand = &nand_info[CONFIG_NAND_FLASH_INFO_IDX];
	if (offset > partition->nand->size ||
	    length > partition->nand->size - offset ||
	    offset % partition->nand->erasesize ||
	    length % partition->nand->erasesize) {
		printf("WXR NAND: SMEM partition '%s' has invalid bounds\n", name);
		wxr_error_set(WXR_ERROR_TARGET, name, "SMEM partition bounds",
			      -EINVAL, 0);
		return -EINVAL;
	}

	partition->offset = offset;
	partition->length = length;
	return 0;
}

static int wxr_attach_nand_partition(const char *name,
				     struct wxr_nand_partition *partition)
{
	struct mtd_device *device;
	struct part_info *part;
	u8 part_number;
	int ret;

	ret = wxr_resolve_nand_partition(name, partition);
	if (ret)
		return ret;
	ret = mtdparts_init();
	if (ret || find_dev_and_part(name, &device, &part_number, &part)) {
		printf("WXR NAND: mtdparts partition '%s' is unavailable\n", name);
		wxr_error_set(WXR_ERROR_TARGET, name, "mtdparts partition lookup",
			      -ENOENT, 0);
		return -ENOENT;
	}
	if (device->id->type != MTD_DEV_TYPE_NAND ||
	    device->id->num != CONFIG_NAND_FLASH_INFO_IDX ||
	    part->offset != partition->offset ||
	    part->size != partition->length) {
		printf("WXR NAND: SMEM and mtdparts disagree for '%s'\n", name);
		wxr_error_set(WXR_ERROR_TARGET, name, "SMEM/mtdparts boundary",
			      -EINVAL, 0);
		return -EINVAL;
	}
	ret = ubi_part((char *)name, NULL);
	if (ret) {
		printf("WXR NAND: cannot attach UBI partition '%s'\n", name);
		wxr_error_set(WXR_ERROR_TARGET, name, "UBI attach", ret, 0);
	}
	return ret;
}

static int wxr_get_volume_capacity(const char *name, long long *capacity)
{
	long long used;
	int ret;

	ret = ubi_volume_get_info((char *)name, &used, capacity);
	if (ret) {
		*capacity = 0;
		return ret;
	}

	return 0;
}

static int wxr_prepare_volume(const char *name, size_t length)
{
	long long available;
	long long capacity;
	int exists;
	int ret;

	exists = !wxr_get_volume_capacity(name, &capacity);
	available = ubi_get_available_bytes();
	if (available < 0) {
		wxr_error_set(WXR_ERROR_IO, name, "UBI capacity query",
			      available, 0);
		return available;
	}
	if ((u64)length > (u64)available + capacity) {
		printf("WXR NAND: volume '%s' has insufficient capacity\n", name);
		wxr_error_set(WXR_ERROR_CAPACITY, name, "UBI volume capacity",
			      -ENOSPC, 0);
		return -ENOSPC;
	}

	if (exists) {
		ret = ubi_volume_remove((char *)name);
		if (ret) {
			wxr_error_set(WXR_ERROR_IO, name, "UBI volume replacement",
				      -ret, 1);
			return -ret;
		}
	}

	ret = ubi_volume_create((char *)name, length, 1);
	if (ret)
		wxr_error_set(WXR_ERROR_IO, name, "UBI volume creation",
			      -ret, 1);
	return ret ? -ret : 0;
}

static int wxr_write_volume(const char *name, const struct wxr_member *member)
{
	const u8 *data = member->data;
	size_t remaining = member->length;
	int first = 1;

	while (remaining) {
		size_t chunk = min(remaining, (size_t)WXR_NAND_IO_BUFFER_SIZE);
		int ret;

		if (first) {
			ret = ubi_volume_begin_write((char *)name, (void *)data,
						     chunk, member->length);
			first = 0;
		} else {
			ret = ubi_volume_continue_write((char *)name,
							(void *)data, chunk);
		}
		if (ret)
			return -ret;

		data += chunk;
		remaining -= chunk;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	return 0;
}

static int wxr_reformat_nand_partition(
		const char *name, struct wxr_nand_partition *partition)
{
	nand_erase_options_t options = {
		.length = partition->length,
		.offset = partition->offset,
		.quiet = 0,
		.jffs2 = 0,
		.scrub = 0,
		.spread = 0,
		.lim = partition->length,
	};
	int ret;

	printf("WXR NAND: rebuilding invalid UBI partition '%s'\n", name);
	ret = nand_erase_opts(partition->nand, &options);
	if (ret) {
		if (ret > 0)
			ret = -ret;
		wxr_error_set(WXR_ERROR_IO, name, "partition erase", ret, 1);
		return ret;
	}

	ret = wxr_attach_nand_partition(name, partition);
	if (ret) {
		if (ret > 0)
			ret = -ret;
		wxr_error_set(WXR_ERROR_IO, name, "UBI reformat attach", ret, 1);
		return ret;
	}

	return 0;
}

static int wxr_rebuild_kernel_partition(
		const char *name, struct wxr_nand_partition *partition,
		size_t kernel_length)
{
	struct wxr_member fw_hash = {
		.data = wxr_nand_fw_hash_reset,
		.length = sizeof(wxr_nand_fw_hash_reset) - 1,
	};
	int ret;

	ret = wxr_reformat_nand_partition(name, partition);
	if (ret)
		return ret;

	ret = ubi_volume_create(WXR_NAND_COMPAT_ROOTFS_VOLUME, 1, 1);
	if (ret)
		goto create_failed;
	ret = ubi_volume_create(WXR_NAND_FW_HASH_VOLUME, 1, 0);
	if (ret)
		goto create_failed;
	ret = wxr_write_volume(WXR_NAND_FW_HASH_VOLUME, &fw_hash);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, name, "fw_hash write", ret, 1);
		return ret;
	}
	ret = ubi_volume_create(WXR_NAND_KERNEL_VOLUME, kernel_length, 1);
	if (ret)
		goto create_failed;

	return 0;

create_failed:
	ret = -ret;
	wxr_error_set(ret == -ENOSPC ? WXR_ERROR_CAPACITY : WXR_ERROR_IO,
		      name, "UBI volume creation", ret, 1);
	return ret;
}

static int wxr_hash_volume(const char *name, size_t length,
			   u8 digest[SHA256_SUM_LEN])
{
	sha256_context context;
	u8 *buffer;
	size_t offset = 0;

	buffer = memalign(ARCH_DMA_MINALIGN, WXR_NAND_IO_BUFFER_SIZE);
	if (!buffer)
		return -ENOMEM;

	sha256_starts(&context);
	while (offset < length) {
		size_t chunk = min(length - offset,
				   (size_t)WXR_NAND_IO_BUFFER_SIZE);
		int ret = ubi_volume_read_at((char *)name, offset, buffer, chunk);

		if (ret) {
			free(buffer);
			return -ret;
		}
		sha256_update(&context, buffer, chunk);
		offset += chunk;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	sha256_finish(&context, digest);
	free(buffer);
	return 0;
}

static int wxr_verify_volume_hash(const char *name, size_t length,
				  const u8 expected[SHA256_SUM_LEN])
{
	u8 actual[SHA256_SUM_LEN];
	long long capacity;
	int ret;

	ret = ubi_volume_get_info((char *)name, NULL, &capacity);
	if (ret)
		return -ret;
	if ((u64)capacity < (u64)length)
		return -EFBIG;

	ret = wxr_hash_volume(name, length, actual);
	if (ret)
		return ret;
	return memcmp(actual, expected, SHA256_SUM_LEN) ? -EIO : 0;
}

static int wxr_load_volume(const char *name, size_t length,
			   struct wxr_image *image)
{
	size_t offset = 0;
	int ret;

	ret = wxr_set_input(length, image);
	if (ret)
		return ret;

	while (offset < length) {
		size_t chunk = min(length - offset,
				   (size_t)WXR_NAND_IO_BUFFER_SIZE);

		ret = ubi_volume_read_at((char *)name, offset,
					 (void *)(WXR_INPUT_ADDRESS + offset), chunk);
		if (ret)
			return -ret;
		offset += chunk;
#ifdef CONFIG_SHOW_ACTIVITY
		show_activity(0);
#endif
		WATCHDOG_RESET();
	}

	return 0;
}

static int wxr_verify_recovery_volume(size_t length,
				      const u8 expected[SHA256_SUM_LEN])
{
	struct wxr_image readback;
	u8 actual[SHA256_SUM_LEN];
	int ret;

	ret = wxr_load_volume(WXR_NAND_KERNEL_VOLUME, length, &readback);
	if (ret)
		return ret;
	wxr_sha256((const void *)readback.address, readback.length, actual);
	if (memcmp(actual, expected, SHA256_SUM_LEN))
		return -EIO;
	return wxr_validate_recovery(&readback);
}

int wxr_nand_boot_production(void)
{
	int ret;

	wxr_set_status(WXR_STATUS_SUCCESS);
	ret = run_command("bootipq", 0);
	if (ret == CMD_RET_SUCCESS)
		return 0;

	puts("WXR NAND production: bootipq failed\n");
	wxr_set_status(WXR_STATUS_FAILURE);
	wxr_error_set(WXR_ERROR_BOOT, "NAND production", "bootipq handoff",
		      -EIO, 0);
	return -EIO;
}

int wxr_nand_boot_recovery(cmd_tbl_t *cmdtp)
{
	struct wxr_nand_partition partition;
	struct wxr_image image;
	long long capacity;
	long long used;
	int ret;

	ret = wxr_attach_nand_partition(WXR_NAND_RECOVERY_PARTITION,
					&partition);
	if (ret)
		return ret;
	ret = ubi_volume_get_info(WXR_NAND_KERNEL_VOLUME, &used, &capacity);
	if (ret) {
		wxr_error_set(WXR_ERROR_TARGET, "NAND recovery", "kernel volume",
			      -ret, 0);
		return -ret;
	}
	if (used <= 0 || used > WXR_INPUT_MAX_SIZE || used > capacity) {
		puts("WXR NAND recovery: kernel volume length is invalid\n");
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND recovery",
			      "kernel volume length", -EFBIG, 0);
		return -EFBIG;
	}

	wxr_set_status(WXR_STATUS_RECEIVING);
	ret = wxr_load_volume(WXR_NAND_KERNEL_VOLUME, used, &image);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND recovery", "kernel volume read",
			      ret, 0);
		return ret;
	}
	wxr_set_status(WXR_STATUS_VALIDATING);
	return wxr_boot_recovery(cmdtp, &image);
}

int wxr_nand_write_recovery(const struct wxr_image *image)
{
	struct wxr_nand_partition partition;
	struct wxr_member member;
	u8 expected[SHA256_SUM_LEN];
	int rebuild = 0;
	int ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_validate_recovery(image);
	if (ret)
		return ret;
	ret = wxr_resolve_nand_partition(WXR_NAND_RECOVERY_PARTITION,
					 &partition);
	if (ret)
		return ret;
	if (image->length > partition.length) {
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND recovery",
			      "partition capacity", -EFBIG, 0);
		return -EFBIG;
	}

	member.data = (const void *)image->address;
	member.length = image->length;
	wxr_sha256(member.data, member.length, expected);

	ret = wxr_attach_nand_partition(WXR_NAND_RECOVERY_PARTITION,
					&partition);
	if (ret == EINVAL)
		rebuild = 1;
	else if (ret)
		return ret;

	wxr_set_status(WXR_STATUS_WRITING);
	if (rebuild)
		ret = wxr_rebuild_kernel_partition(
			WXR_NAND_RECOVERY_PARTITION, &partition, image->length);
	else
		ret = wxr_prepare_volume(WXR_NAND_KERNEL_VOLUME, image->length);
	if (ret)
		return ret;

	ret = wxr_write_volume(WXR_NAND_KERNEL_VOLUME, &member);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND recovery", "kernel volume write",
			      ret, 1);
		return ret;
	}
	ret = wxr_attach_nand_partition(WXR_NAND_RECOVERY_PARTITION,
					&partition);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND recovery", "readback attach",
			      ret, 1);
		return ret;
	}
	ret = wxr_verify_recovery_volume(image->length, expected);
	if (ret) {
		wxr_error_set(WXR_ERROR_VERIFY, "NAND recovery", "readback image",
			      ret, 1);
		return ret;
	}

	wxr_set_status(WXR_STATUS_SUCCESS);
	return 0;
}

static int wxr_precheck_user_volumes(size_t root_length)
{
	u64 reclaimable;
	u64 rounded_root;
	long long root_capacity;
	long long data_capacity;
	long long available;
	long long leb_size;

	wxr_get_volume_capacity(WXR_NAND_ROOTFS_VOLUME, &root_capacity);
	wxr_get_volume_capacity(WXR_NAND_DATA_VOLUME, &data_capacity);
	available = ubi_get_available_bytes();
	if (available < 0) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "user_property capacity query", available, 0);
		return available;
	}
	leb_size = ubi_get_usable_leb_size();
	if (leb_size <= 0) {
		int ret = leb_size ? leb_size : -EINVAL;

		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "user_property LEB size query", ret, 0);
		return ret;
	}
	if ((u64)available > ~(u64)0 - (u64)root_capacity ||
	    (u64)available + (u64)root_capacity >
					~(u64)0 - (u64)data_capacity ||
	    (u64)root_length > ~(u64)0 - ((u64)leb_size - 1)) {
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND production",
			      "user_property capacity arithmetic", -EOVERFLOW, 0);
		return -EOVERFLOW;
	}

	reclaimable = (u64)available + (u64)root_capacity +
		      (u64)data_capacity;
	rounded_root = DIV_ROUND_UP((u64)root_length, (u64)leb_size) *
		       (u64)leb_size;
	if (rounded_root > reclaimable ||
	    reclaimable - rounded_root < (u64)leb_size) {
		puts("WXR NAND production: user_property capacity is insufficient\n");
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND production",
			      "rootfs and rootfs_data LEB capacity", -ENOSPC, 0);
		return -ENOSPC;
	}

	return 0;
}

static int wxr_precheck_kernel_volume(size_t kernel_length)
{
	long long kernel_capacity;
	long long available;

	wxr_get_volume_capacity(WXR_NAND_KERNEL_VOLUME, &kernel_capacity);
	available = ubi_get_available_bytes();
	if (available < 0) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "kernel capacity query", available, 0);
		return available;
	}
	if ((u64)kernel_length > (u64)available + kernel_capacity) {
		puts("WXR NAND production: kernel capacity is insufficient\n");
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND production",
			      "kernel capacity", -ENOSPC, 0);
		return -ENOSPC;
	}

	return 0;
}

static int wxr_remove_volume_if_present(const char *name)
{
	long long capacity;
	int ret;

	if (wxr_get_volume_capacity(name, &capacity))
		return 0;
	ret = ubi_volume_remove((char *)name);
	return ret ? -ret : 0;
}

static int wxr_verify_root_volume(const struct wxr_member *root,
				  const u8 expected[SHA256_SUM_LEN])
{
	u8 header[64];
	struct wxr_member readback;
	u64 bytes_used;
	int ret;

	ret = wxr_verify_volume_hash(WXR_NAND_ROOTFS_VOLUME, root->length,
				     expected);
	if (ret)
		return ret;
	ret = ubi_volume_read_at(WXR_NAND_ROOTFS_VOLUME, 0, header,
				 sizeof(header));
	if (ret)
		return -ret;
	readback.data = header;
	readback.length = root->length;
	return wxr_validate_squashfs(&readback, &bytes_used);
}

static int wxr_verify_kernel_volume(size_t length,
				    const u8 expected[SHA256_SUM_LEN])
{
	struct wxr_image readback;
	struct wxr_member fit;
	int ret;

	ret = wxr_verify_volume_hash(WXR_NAND_KERNEL_VOLUME, length, expected);
	if (ret)
		return ret;
	ret = wxr_load_volume(WXR_NAND_KERNEL_VOLUME, length, &readback);
	if (ret)
		return ret;
	fit.data = (const void *)readback.address;
	fit.length = readback.length;
	return wxr_validate_openwrt_fit(&fit);
}

int wxr_nand_write_production(const struct wxr_image *image)
{
	struct wxr_nand_partition kernel_partition;
	struct wxr_nand_partition user_partition;
	struct wxr_sysupgrade archive;
	u8 kernel_sha256[SHA256_SUM_LEN];
	u8 root_sha256[SHA256_SUM_LEN];
	u64 root_bytes_used;
	long long data_size;
	int rebuild_kernel = 0;
	int rebuild_user = 0;
	int ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_parse_sysupgrade(image, WXR_SYSUPGRADE_NAND, &archive);
	if (ret)
		return ret;
	ret = wxr_validate_openwrt_fit(&archive.kernel);
	if (ret)
		return ret;
	ret = wxr_validate_squashfs(&archive.root, &root_bytes_used);
	if (ret)
		return ret;

	ret = wxr_resolve_nand_partition(WXR_NAND_PRODUCTION_PARTITION,
					 &kernel_partition);
	if (ret)
		return ret;
	ret = wxr_resolve_nand_partition(WXR_NAND_USER_PARTITION,
					 &user_partition);
	if (ret)
		return ret;
	if (archive.kernel.length > kernel_partition.length ||
	    archive.root.length > user_partition.length) {
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND production",
			      "partition capacity", -EFBIG, 0);
		return -EFBIG;
	}

	ret = wxr_attach_nand_partition(WXR_NAND_USER_PARTITION,
					&user_partition);
	if (ret == EINVAL)
		rebuild_user = 1;
	else if (ret)
		return ret;
	else {
		ret = wxr_precheck_user_volumes(archive.root.length);
		if (ret)
			return ret;
	}
	ret = wxr_attach_nand_partition(WXR_NAND_PRODUCTION_PARTITION,
					&kernel_partition);
	if (ret == EINVAL)
		rebuild_kernel = 1;
	else if (ret)
		return ret;
	else {
		ret = wxr_precheck_kernel_volume(archive.kernel.length);
		if (ret)
			return ret;
	}

	wxr_sha256(archive.kernel.data, archive.kernel.length, kernel_sha256);
	wxr_sha256(archive.root.data, archive.root.length, root_sha256);
	wxr_set_status(WXR_STATUS_WRITING);

	if (rebuild_user) {
		ret = wxr_reformat_nand_partition(WXR_NAND_USER_PARTITION,
						  &user_partition);
		if (ret)
			return ret;
		ret = wxr_precheck_user_volumes(archive.root.length);
		if (ret) {
			wxr_error_set(ret == -ENOSPC ? WXR_ERROR_CAPACITY :
				      WXR_ERROR_IO, "NAND production",
				      "reformatted user_property capacity", ret, 1);
			return ret;
		}
	} else {
		ret = wxr_attach_nand_partition(WXR_NAND_USER_PARTITION,
						&user_partition);
		if (ret)
			return ret;
	}
	ret = wxr_remove_volume_if_present(WXR_NAND_DATA_VOLUME);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "overlay volume removal", ret, 1);
		return ret;
	}
	ret = wxr_remove_volume_if_present(WXR_NAND_ROOTFS_VOLUME);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "rootfs volume removal", ret, 1);
		return ret;
	}
	ret = ubi_volume_create(WXR_NAND_ROOTFS_VOLUME, archive.root.length, 1);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "rootfs volume creation", -ret, 1);
		return -ret;
	}
	ret = wxr_write_volume(WXR_NAND_ROOTFS_VOLUME, &archive.root);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production", "rootfs write",
			      ret, 1);
		return ret;
	}
	ret = wxr_verify_root_volume(&archive.root, root_sha256);
	if (ret) {
		wxr_error_set(WXR_ERROR_VERIFY, "NAND production",
			      "rootfs readback", ret, 1);
		return ret;
	}

	data_size = ubi_get_available_bytes();
	if (data_size <= 0) {
		wxr_error_set(WXR_ERROR_CAPACITY, "NAND production",
			      "overlay capacity", -ENOSPC, 1);
		return -ENOSPC;
	}
	ret = ubi_volume_create(WXR_NAND_DATA_VOLUME, data_size, 1);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production",
			      "overlay volume creation", -ret, 1);
		return -ret;
	}

	if (rebuild_kernel) {
		ret = wxr_rebuild_kernel_partition(
			WXR_NAND_PRODUCTION_PARTITION, &kernel_partition,
			archive.kernel.length);
		if (ret)
			return ret;
	} else {
		ret = wxr_attach_nand_partition(WXR_NAND_PRODUCTION_PARTITION,
						&kernel_partition);
		if (ret) {
			wxr_error_set(WXR_ERROR_IO, "NAND production",
				      "kernel write attach", ret, 1);
			return ret;
		}
		ret = wxr_prepare_volume(WXR_NAND_KERNEL_VOLUME,
					 archive.kernel.length);
		if (ret) {
			wxr_error_set(ret == -ENOSPC ? WXR_ERROR_CAPACITY :
				      WXR_ERROR_IO, "NAND production",
				      "kernel volume preparation", ret, 1);
			return ret;
		}
	}
	ret = wxr_write_volume(WXR_NAND_KERNEL_VOLUME, &archive.kernel);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND production", "kernel write",
			      ret, 1);
		return ret;
	}
	ret = wxr_verify_kernel_volume(archive.kernel.length, kernel_sha256);
	if (ret) {
		wxr_error_set(WXR_ERROR_VERIFY, "NAND production",
			      "kernel readback", ret, 1);
		return ret;
	}

	wxr_set_status(WXR_STATUS_SUCCESS);
	return 0;
}

int wxr_nand_reset_configuration(void)
{
	struct wxr_nand_partition partition;
	int ret;

	wxr_set_status(WXR_STATUS_VALIDATING);
	ret = wxr_attach_nand_partition(WXR_NAND_USER_PARTITION, &partition);
	if (ret)
		return ret;
	ret = ubi_volume_get_info(WXR_NAND_DATA_VOLUME, NULL, NULL);
	if (ret) {
		wxr_error_set(WXR_ERROR_TARGET, "NAND configuration",
			      "rootfs_data volume lookup", -ret, 0);
		return -ret;
	}

	wxr_set_status(WXR_STATUS_WRITING);
	ret = ubi_volume_clear(WXR_NAND_DATA_VOLUME);
	if (ret) {
		wxr_error_set(WXR_ERROR_IO, "NAND configuration",
			      "rootfs_data zero-length update", -ret, 1);
		wxr_set_status(WXR_STATUS_FAILURE);
		return -ret;
	}

	wxr_set_status(WXR_STATUS_SUCCESS);
	return 0;
}
