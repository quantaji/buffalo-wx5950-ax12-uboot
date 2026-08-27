/*
 * Copyright (c) 2016-2017, 2020 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <asm/errno.h>
#include <nand.h>
#include <part.h>
#include <mmc.h>
#include <asm/arch-qca-common/smem.h>
#include <asm/arch-qca-common/qca_common.h>
#include <sdhci.h>
#ifdef CONFIG_IPQ_TINY_SPI_NOR
#include <spi.h>
#include <spi_flash.h>
#endif
#if defined(CONFIG_ART_COMPRESSED) &&	\
	(defined(CONFIG_GZIP) || defined(CONFIG_LZMA))
#ifndef CONFIG_COMPRESSED_LOAD_ADDR
#define CONFIG_COMPRESSED_LOAD_ADDR CONFIG_SYS_LOAD_ADDR
#endif
#include <mapmem.h>
#include <lzma/LzmaTools.h>
#endif
#ifdef CONFIG_QCA_MMC
#ifndef CONFIG_SDHCI_SUPPORT
extern qca_mmc mmc_host;
#else
extern  struct sdhci_host mmc_host;
#endif
#endif

#ifdef CONFIG_BUFFALO_WXR5950AX12
#define WXR_ORGDATA_IDENTITY_SIZE	0x32
#define WXR_ORGDATA_MAC_OFFSET		0x20

static int wxr_read_orgdata(char *partition, uchar *identity)
{
	u32 start_blocks;
	u32 size_blocks;
	u32 flash_type;
	u32 length = WXR_ORGDATA_IDENTITY_SIZE;
	u64 partition_size;
	loff_t offset;
	int ret;

	if (qca_smem_flash_info.flash_type != SMEM_BOOT_NAND_FLASH &&
	    qca_smem_flash_info.flash_type != SMEM_BOOT_QSPI_NAND_FLASH)
		return -ENODEV;

	ret = smem_getpart(partition, &start_blocks, &size_blocks);
	if (ret < 0)
		return ret;

	partition_size =
		(u64)qca_smem_flash_info.flash_block_size * size_blocks;
	if (partition_size < WXR_ORGDATA_IDENTITY_SIZE)
		return -EINVAL;

	flash_type = CONFIG_NAND_FLASH_INFO_IDX;
	offset = (loff_t)qca_smem_flash_info.flash_block_size * start_blocks;
	ret = nand_read(&nand_info[flash_type], offset, &length, identity);
	if (ret)
		return ret;
	if (length != WXR_ORGDATA_IDENTITY_SIZE)
		return -EIO;

	return 0;
}

static void wxr_mac_add(const uchar *base, uint increment, uchar *result)
{
	int i;
	uint value = increment;

	memcpy(result, base, 6);
	for (i = 5; i >= 0 && value; i--) {
		value += result[i];
		result[i] = value & 0xff;
		value >>= 8;
	}
}

static int wxr_get_orgdata_mac(uchar *mac)
{
	static const char model[] = "WXR-5950AX12";
	uchar identity[WXR_ORGDATA_IDENTITY_SIZE];
	uchar identity_copy[WXR_ORGDATA_IDENTITY_SIZE];
	uchar expected[6];
	int ret;

	ret = wxr_read_orgdata("0:ORGDATA", identity);
	if (ret)
		return ret;
	ret = wxr_read_orgdata("0:ORGDATA_1", identity_copy);
	if (ret)
		return ret;

	if (memcmp(identity, identity_copy, sizeof(identity))) {
		puts("WXR identity: ORGDATA copies differ\n");
		return -EINVAL;
	}
	if (memcmp(identity, model, sizeof(model))) {
		puts("WXR identity: model in ORGDATA is invalid\n");
		return -EINVAL;
	}
	if (!is_valid_ethaddr(identity + WXR_ORGDATA_MAC_OFFSET) ||
	    !is_valid_ethaddr(identity + WXR_ORGDATA_MAC_OFFSET + 6) ||
	    !is_valid_ethaddr(identity + WXR_ORGDATA_MAC_OFFSET + 12)) {
		puts("WXR identity: ORGDATA contains an invalid MAC address\n");
		return -EINVAL;
	}

	wxr_mac_add(identity + WXR_ORGDATA_MAC_OFFSET, 8, expected);
	if (memcmp(expected, identity + WXR_ORGDATA_MAC_OFFSET + 6, 6)) {
		puts("WXR identity: WLAN0 MAC is not wired MAC + 8\n");
		return -EINVAL;
	}
	wxr_mac_add(identity + WXR_ORGDATA_MAC_OFFSET, 16, expected);
	if (memcmp(expected, identity + WXR_ORGDATA_MAC_OFFSET + 12, 6)) {
		puts("WXR identity: WLAN1 MAC is not wired MAC + 16\n");
		return -EINVAL;
	}

	memcpy(mac, identity + WXR_ORGDATA_MAC_OFFSET, 6);
	return 0;
}
#endif

/*
 * Gets the ethernet address from the ART partition table and return the value
 */
int get_eth_mac_address(uchar *enetaddr, uint no_of_macs)
{
	s32 ret = 0 ;
	u32 start_blocks;
	u32 size_blocks;
	u32 length = (6 * no_of_macs);
	u32 flash_type;
	loff_t art_offset;
	qca_smem_flash_info_t *sfi = &qca_smem_flash_info;
#ifdef CONFIG_QCA_MMC
	block_dev_desc_t *blk_dev;
	disk_partition_t disk_info;
	struct mmc *mmc;
	char mmc_blks[512];
#endif
#ifdef CONFIG_IPQ_TINY_SPI_NOR
	struct spi_flash *flash = NULL;
#if defined(CONFIG_ART_COMPRESSED) && (defined(CONFIG_GZIP) || defined(CONFIG_LZMA))
	void *load_buf, *image_buf;
	unsigned long img_size;
#endif
#endif

	if (sfi->flash_type != SMEM_BOOT_MMC_FLASH) {
		if (qca_smem_flash_info.flash_type == SMEM_BOOT_SPI_FLASH)
			flash_type = CONFIG_SPI_FLASH_INFO_IDX;
		else if (qca_smem_flash_info.flash_type == SMEM_BOOT_NAND_FLASH)
			flash_type = CONFIG_NAND_FLASH_INFO_IDX;
		else if (qca_smem_flash_info.flash_type == SMEM_BOOT_QSPI_NAND_FLASH)
			flash_type = CONFIG_NAND_FLASH_INFO_IDX;
		else {
			printf("Unknown flash type\n");
			return -EINVAL;
		}

		ret = smem_getpart("0:ART", &start_blocks, &size_blocks);
		if (ret < 0) {
			printf("No ART partition found\n");
			return ret;
		}

		/*
		 * ART partition 0th position will contain Mac address.
		 */
		art_offset =
		((loff_t) qca_smem_flash_info.flash_block_size * start_blocks);

#ifdef CONFIG_IPQ_TINY_SPI_NOR
		flash = spi_flash_probe(CONFIG_SF_DEFAULT_BUS, CONFIG_SF_DEFAULT_CS,
				CONFIG_SF_DEFAULT_SPEED, CONFIG_SF_DEFAULT_MODE);
		if (flash == NULL){
			printf("No SPI flash device found\n");
			ret = -1;
		} else {
#if defined(CONFIG_ART_COMPRESSED) && (defined(CONFIG_GZIP) || defined(CONFIG_LZMA))
		image_buf = map_sysmem(CONFIG_COMPRESSED_LOAD_ADDR, 0);
		load_buf = map_sysmem(CONFIG_COMPRESSED_LOAD_ADDR + 0x100000, 0);
		img_size = qca_smem_flash_info.flash_block_size * size_blocks;
		ret = spi_flash_read(flash, art_offset, img_size, image_buf);
		if (ret == 0) {
			ret = -1;
#ifdef CONFIG_GZIP
			ret = gunzip(load_buf, img_size, image_buf, &img_size);
#endif
#ifdef CONFIG_LZMA
			if (ret != 0)
				ret = lzmaBuffToBuffDecompress(load_buf,
					(SizeT *)&img_size,
					image_buf,
					(SizeT)img_size);
#endif
			if (ret == 0) {
				memcpy(enetaddr, load_buf, length);
			} else {
				printf("Invalid compression type..\n");
				ret = -1;
			}
		}
#else
		ret = spi_flash_read(flash, art_offset, length, enetaddr);
#endif
		}
		/*
		 * Avoid unused warning
		 */
		(void)flash_type;
#else
		ret = nand_read(&nand_info[flash_type],
				art_offset, &length, enetaddr);
#endif
		if (ret < 0)
			printf("ART partition read failed..\n");
#ifdef CONFIG_QCA_MMC
	} else {
		blk_dev = mmc_get_dev(mmc_host.dev_num);
		ret = get_partition_info_efi_by_name(blk_dev, "0:ART", &disk_info);
		/*
		 * ART partition 0th position will contain MAC address.
		 * Read 1 block.
		 */
		if (ret == 0) {
			mmc = mmc_host.mmc;
			ret = mmc->block_dev.block_read
				(mmc_host.dev_num, disk_info.start,
						1, mmc_blks);
			memcpy(enetaddr, mmc_blks, length);
                }
		if (ret < 0)
			printf("ART partition read failed..\n");
#endif
	}
	return ret;
}

void set_ethmac_addr(void)
{
#ifdef CONFIG_BUFFALO_WXR5950AX12
	uchar env_mac[6];
	uchar org_mac[6];
	int org_valid;

	org_valid = wxr_get_orgdata_mac(org_mac) == 0;
	if (eth_getenv_enetaddr("ethaddr", env_mac)) {
		printf("WXR Ethernet MAC from APPSBLENV: %pM\n", env_mac);
		if (org_valid && memcmp(env_mac, org_mac, 6))
			printf("WXR identity warning: APPSBLENV %pM differs from ORGDATA %pM\n",
			       env_mac, org_mac);
		return;
	}

	if (org_valid) {
		eth_setenv_enetaddr("ethaddr", org_mac);
		printf("WXR Ethernet MAC recovered in memory from ORGDATA: %pM\n",
		       org_mac);
		return;
	}

	puts("WXR Ethernet disabled: no valid APPSBLENV or ORGDATA MAC\n");
#else
	int i, ret;
	uchar enetaddr[CONFIG_IPQ_NO_MACS * 6];
	uchar *mac_addr;
	char ethaddr[16] = "ethaddr";
	char mac[64];
	/* Get the MAC address from ART partition */
	ret = get_eth_mac_address(enetaddr, CONFIG_IPQ_NO_MACS);
	for (i = 0; (ret >= 0) && (i < CONFIG_IPQ_NO_MACS); i++) {
		mac_addr = &enetaddr[i * 6];
		if (!is_valid_ethaddr(mac_addr)) {
			printf("eth%d MAC Address from ART is not valid\n", i);
		} else {
			/*
			 * U-Boot uses these to patch the 'local-mac-address'
			 * dts entry for the ethernet entries, which in turn
			 * will be picked up by the HLOS driver
			 */
			snprintf(mac, sizeof(mac), "%x:%x:%x:%x:%x:%x",
					mac_addr[0], mac_addr[1],
					mac_addr[2], mac_addr[3],
					mac_addr[4], mac_addr[5]);
			setenv(ethaddr, mac);
		}
		snprintf(ethaddr, sizeof(ethaddr), "eth%daddr", (i + 1));
	}
#endif
}
