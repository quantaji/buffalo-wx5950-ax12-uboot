/*
 * Copyright (c) 2001 William L. Pitts
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are freely
 * permitted provided that the above copyright notice and this
 * paragraph and the following disclaimer are duplicated in all
 * such forms.
 *
 * This software is provided "AS IS" and without any express or
 * implied warranties, including, without limitation, the implied
 * warranties of merchantability and fitness for a particular
 * purpose.
 */

#include <common.h>
#include <bootm.h>
#include <command.h>
#include <elf.h>
#include <net.h>
#include <vxworks.h>
#ifdef CONFIG_ARM
#include <asm/armv7.h>
#include <asm/u-boot-arm.h>
#endif
#ifdef CONFIG_X86
#include <asm/e820.h>
#include <linux/linkage.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

static int bootelf_range_end(ulong start, ulong size, ulong *end)
{
	*end = start + size;
	if (*end < start)
		return -1;

	return 0;
}

static int bootelf_ranges_overlap(ulong first_start, ulong first_end,
				  ulong second_start, ulong second_end)
{
	return first_start < second_end && second_start < first_end;
}

/*
 * A very simple elf loader, assumes the image is valid, returns the
 * entry point address.
 */
static int load_elf_image_phdr(ulong addr, ulong *entry)
{
	Elf32_Ehdr *ehdr;
	Elf32_Phdr *phdr;
	int i;

	ehdr = (Elf32_Ehdr *)addr;
	phdr = (Elf32_Phdr *)(addr + ehdr->e_phoff);

	for (i = 0; i < ehdr->e_phnum; i++) {
		void *dst;
		void *src;

		if (phdr[i].p_type != PT_LOAD)
			continue;

		if (phdr[i].p_filesz > phdr[i].p_memsz) {
			printf("## PT_LOAD %d has filesz larger than memsz\n", i);
			return 1;
		}

		dst = (void *)(uintptr_t)phdr[i].p_paddr;
		src = (void *)(addr + phdr[i].p_offset);
		debug("Loading PT_LOAD %d to 0x%p (%u/%u bytes)\n",
		      i, dst, phdr[i].p_filesz, phdr[i].p_memsz);

		if (phdr[i].p_filesz)
			memcpy(dst, src, phdr[i].p_filesz);

		if (phdr[i].p_memsz > phdr[i].p_filesz)
			memset(dst + phdr[i].p_filesz, 0,
			       phdr[i].p_memsz - phdr[i].p_filesz);

		if (phdr[i].p_memsz)
			flush_cache((ulong)dst, phdr[i].p_memsz);
	}

	*entry = ehdr->e_entry;
	return 0;
}

static unsigned long load_elf_image_shdr(unsigned long addr)
{
	Elf32_Ehdr *ehdr; /* Elf header structure pointer */
	Elf32_Shdr *shdr; /* Section header structure pointer */
	unsigned char *strtab = 0; /* String table pointer */
	unsigned char *image; /* Binary image pointer */
	int i; /* Loop counter */

	ehdr = (Elf32_Ehdr *)addr;

	/* Find the section header string table for output info */
	shdr = (Elf32_Shdr *)(addr + ehdr->e_shoff +
			     (ehdr->e_shstrndx * sizeof(Elf32_Shdr)));

	if (shdr->sh_type == SHT_STRTAB)
		strtab = (unsigned char *)(addr + shdr->sh_offset);

	/* Load each appropriate section */
	for (i = 0; i < ehdr->e_shnum; ++i) {
		shdr = (Elf32_Shdr *)(addr + ehdr->e_shoff +
				     (i * sizeof(Elf32_Shdr)));

		if (!(shdr->sh_flags & SHF_ALLOC) ||
		    shdr->sh_addr == 0 || shdr->sh_size == 0) {
			continue;
		}

		if (strtab) {
			debug("%sing %s @ 0x%08lx (%ld bytes)\n",
			      (shdr->sh_type == SHT_NOBITS) ? "Clear" : "Load",
			       &strtab[shdr->sh_name],
			       (unsigned long)shdr->sh_addr,
			       (long)shdr->sh_size);
		}

		if (shdr->sh_type == SHT_NOBITS) {
			memset((void *)(uintptr_t)shdr->sh_addr, 0,
			       shdr->sh_size);
		} else {
			image = (unsigned char *)addr + shdr->sh_offset;
			memcpy((void *)(uintptr_t)shdr->sh_addr,
			       (const void *)image, shdr->sh_size);
		}
		flush_cache(shdr->sh_addr, shdr->sh_size);
	}

	return ehdr->e_entry;
}

/* Allow ports to override the default behavior */
static unsigned long do_bootelf_exec(ulong (*entry)(int, char * const[]),
				     int argc, char * const argv[])
{
	unsigned long ret;

	/*
	 * QNX images require the data cache is disabled.
	 * Data cache is already flushed, so just turn it off.
	 */
	int dcache = dcache_status();
	if (dcache)
		dcache_disable();

	/*
	 * pass address parameter as argv[0] (aka command name),
	 * and all remaining args
	 */
	ret = entry(argc, argv);

	if (dcache)
		dcache_enable();

	return ret;
}

static int validate_uboot_elf(ulong addr, ulong *entry)
{
	Elf32_Ehdr *ehdr = (Elf32_Ehdr *)addr;
	Elf32_Phdr *phdr;
	ulong phdr_addr;
	ulong phdr_end;
	ulong image_end;
	ulong runtime_start;
	ulong runtime_end;
	ulong stack_marker;
	int executable_entry = 0;
	int load_segments = 0;
	int i;

	if (!IS_ELF(*ehdr)) {
		printf("## No ELF image at address 0x%08lx\n", addr);
		return 1;
	}

	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
	    ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
	    ehdr->e_machine != EM_ARM) {
		puts("## U-Boot handoff requires a 32-bit little-endian ARM ELF\n");
		return 1;
	}

	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
		puts("## U-Boot handoff requires an ET_EXEC or ET_DYN ELF\n");
		return 1;
	}

	if (ehdr->e_ehsize != sizeof(*ehdr) ||
	    ehdr->e_phentsize != sizeof(Elf32_Phdr) ||
	    ehdr->e_phnum == 0) {
		puts("## Invalid ELF program-header description\n");
		return 1;
	}

	if (bootelf_range_end(addr, sizeof(*ehdr), &image_end) ||
	    bootelf_range_end(addr, ehdr->e_phoff, &phdr_addr) ||
	    bootelf_range_end(phdr_addr,
			      ehdr->e_phnum * sizeof(Elf32_Phdr),
			      &phdr_end)) {
		puts("## ELF program-header range overflows memory\n");
		return 1;
	}

	if (phdr_end > image_end)
		image_end = phdr_end;
	phdr = (Elf32_Phdr *)phdr_addr;

	/* Establish the complete source range before checking destinations. */
	for (i = 0; i < ehdr->e_phnum; i++) {
		ulong source_start;
		ulong source_end;

		if (bootelf_range_end(addr, phdr[i].p_offset,
				      &source_start) ||
		    bootelf_range_end(source_start, phdr[i].p_filesz,
				      &source_end)) {
			puts("## ELF source segment range overflows memory\n");
			return 1;
		}

		if (source_end > image_end)
			image_end = source_end;
	}

	runtime_start = min(gd->start_addr_sp, (ulong)&stack_marker);
	if (bootelf_range_end(gd->relocaddr, gd->mon_len, &runtime_end)) {
		puts("## Current U-Boot runtime range is invalid\n");
		return 1;
	}

	for (i = 0; i < ehdr->e_phnum; i++) {
		ulong destination_start;
		ulong destination_end;
#ifdef CONFIG_NR_DRAM_BANKS
		int destination_in_dram = 0;
		int bank;
#endif

		if (phdr[i].p_type != PT_LOAD)
			continue;

		load_segments++;
		if (phdr[i].p_filesz > phdr[i].p_memsz) {
			printf("## PT_LOAD %d has filesz larger than memsz\n", i);
			return 1;
		}

		destination_start = phdr[i].p_paddr;
		if (bootelf_range_end(destination_start, phdr[i].p_memsz,
				      &destination_end)) {
			printf("## PT_LOAD %d destination range overflows memory\n",
			       i);
			return 1;
		}

		if (phdr[i].p_memsz) {
#ifndef CONFIG_NR_DRAM_BANKS
			puts("## Board does not describe usable DRAM\n");
			return 1;
#else
			if (!gd->bd) {
				puts("## Board DRAM information is unavailable\n");
				return 1;
			}

			for (bank = 0; bank < CONFIG_NR_DRAM_BANKS; bank++) {
				ulong bank_start;
				ulong bank_end;

				if (!gd->bd->bi_dram[bank].size)
					continue;

				bank_start = gd->bd->bi_dram[bank].start;
				if (bootelf_range_end(
					    bank_start,
					    gd->bd->bi_dram[bank].size,
					    &bank_end)) {
					puts("## Board DRAM range overflows memory\n");
					return 1;
				}

				if (destination_start >= bank_start &&
				    destination_end <= bank_end) {
					destination_in_dram = 1;
					break;
				}
			}

			if (!destination_in_dram) {
				printf("## PT_LOAD %d is outside usable DRAM\n", i);
				return 1;
			}
#endif
		}

		if (bootelf_ranges_overlap(destination_start, destination_end,
					   runtime_start, runtime_end)) {
			printf("## PT_LOAD %d overlaps the running U-Boot\n", i);
			return 1;
		}

		if (bootelf_ranges_overlap(destination_start, destination_end,
					   addr, image_end)) {
			printf("## PT_LOAD %d overlaps its source ELF image\n", i);
			return 1;
		}

		if ((phdr[i].p_flags & PF_X) &&
		    ehdr->e_entry >= destination_start &&
		    ehdr->e_entry < destination_end)
			executable_entry = 1;
	}

	if (!load_segments) {
		puts("## ELF image contains no PT_LOAD segment\n");
		return 1;
	}

	if (!executable_entry) {
		puts("## ELF entry is outside executable PT_LOAD segments\n");
		return 1;
	}

	*entry = ehdr->e_entry;
	return 0;
}

int __weak board_uboot_handoff_prepare(void)
{
	return 0;
}

#ifdef CONFIG_ARM
static void __noreturn bootelf_start_uboot(ulong entry)
{
	void (*uboot_entry)(void) = (void (*)(void))entry;

#ifdef CONFIG_CMD_NET
	eth_halt();
#endif
	bootm_disable_interrupts();
	cleanup_before_linux();
	DSB;
	ISB;

	uboot_entry();

	reset_cpu(0);
	hang();
}
#endif

/*
 * Determine if a valid ELF image exists at the given memory location.
 * First look at the ELF header magic field, then make sure that it is
 * executable.
 */
int valid_elf_image(unsigned long addr)
{
	Elf32_Ehdr *ehdr; /* Elf header structure pointer */

	ehdr = (Elf32_Ehdr *)addr;

	if (!IS_ELF(*ehdr)) {
		printf("## No elf image at address 0x%08lx\n", addr);
		return 0;
	}

	if (ehdr->e_type != ET_EXEC) {
		printf("## Not a 32-bit elf image at address 0x%08lx\n", addr);
		return 0;
	}

	return 1;
}

/* Interpreter command to boot an arbitrary ELF image from memory */
int do_bootelf(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	unsigned long addr; /* Address of the ELF image */
	unsigned long entry; /* ELF entry point */
	unsigned long rc; /* Return value from user code */
	char *sload, *saddr;
	const char *ep = getenv("autostart");
	int uboot_handoff;

	int rcode = 0;

	sload = saddr = NULL;
	if (argc == 3) {
		sload = argv[1];
		saddr = argv[2];
	} else if (argc == 2) {
		if (argv[1][0] == '-')
			sload = argv[1];
		else
			saddr = argv[1];
	}

	if (saddr)
		addr = simple_strtoul(saddr, NULL, 16);
	else
		addr = load_addr;

	uboot_handoff = sload && !strcmp(sload, "-u");
	if (uboot_handoff) {
#ifndef CONFIG_ARM
		puts("## U-Boot handoff is supported only on ARM\n");
		return 1;
#else
		if (validate_uboot_elf(addr, &entry))
			return 1;

		if (ep && !strcmp(ep, "no"))
			return load_elf_image_phdr(addr, &entry);

		if (board_uboot_handoff_prepare()) {
			puts("## Board rejected the U-Boot handoff state\n");
			return 1;
		}

		if (load_elf_image_phdr(addr, &entry))
			return 1;

		printf("## Starting U-Boot at 0x%08lx ...\n", entry);
		bootelf_start_uboot(entry);
#endif
	}

	if (!valid_elf_image(addr))
		return 1;

	if (sload && sload[1] == 'p') {
		if (load_elf_image_phdr(addr, &entry))
			return 1;
		addr = entry;
	} else {
		addr = load_elf_image_shdr(addr);
	}

	if (ep && !strcmp(ep, "no"))
		return rcode;

	printf("## Starting application at 0x%08lx ...\n", addr);

	/*
	 * pass address parameter as argv[0] (aka command name),
	 * and all remaining args
	 */
	rc = do_bootelf_exec((void *)addr, argc - 1, argv + 1);
	if (rc != 0)
		rcode = 1;

	printf("## Application terminated, rc = 0x%lx\n", rc);

	return rcode;
}

/*
 * Interpreter command to boot VxWorks from a memory image.  The image can
 * be either an ELF image or a raw binary.  Will attempt to setup the
 * bootline and other parameters correctly.
 */
int do_bootvx(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	unsigned long addr; /* Address of image */
	unsigned long bootaddr; /* Address to put the bootline */
	char *bootline; /* Text of the bootline */
	char *tmp; /* Temporary char pointer */
	char build_buf[128]; /* Buffer for building the bootline */
	int ptr = 0;
#ifdef CONFIG_X86
	struct e820info *info;
	struct e820entry *data;
#endif

	/*
	 * Check the loadaddr variable.
	 * If we don't know where the image is then we're done.
	 */
	if (argc < 2)
		addr = load_addr;
	else
		addr = simple_strtoul(argv[1], NULL, 16);

#if defined(CONFIG_CMD_NET)
	/*
	 * Check to see if we need to tftp the image ourselves
	 * before starting
	 */
	if ((argc == 2) && (strcmp(argv[1], "tftp") == 0)) {
		if (net_loop(TFTPGET) <= 0)
			return 1;
		printf("Automatic boot of VxWorks image at address 0x%08lx ...\n",
			addr);
	}
#endif

	/*
	 * This should equate to
	 * NV_RAM_ADRS + NV_BOOT_OFFSET + NV_ENET_OFFSET
	 * from the VxWorks BSP header files.
	 * This will vary from board to board
	 */
#if defined(CONFIG_WALNUT)
	tmp = (char *)CONFIG_SYS_NVRAM_BASE_ADDR + 0x500;
	eth_getenv_enetaddr("ethaddr", (uchar *)build_buf);
	memcpy(tmp, &build_buf[3], 3);
#elif defined(CONFIG_SYS_VXWORKS_MAC_PTR)
	tmp = (char *)CONFIG_SYS_VXWORKS_MAC_PTR;
	eth_getenv_enetaddr("ethaddr", (uchar *)build_buf);
	memcpy(tmp, build_buf, 6);
#else
	puts("## Ethernet MAC address not copied to NV RAM\n");
#endif

	/*
	 * Use bootaddr to find the location in memory that VxWorks
	 * will look for the bootline string. The default value is
	 * (LOCAL_MEM_LOCAL_ADRS + BOOT_LINE_OFFSET) as defined by
	 * VxWorks BSP. For example, on PowerPC it defaults to 0x4200.
	 */
	tmp = getenv("bootaddr");
	if (!tmp) {
		printf("## VxWorks bootline address not specified\n");
	} else {
		bootaddr = simple_strtoul(tmp, NULL, 16);

		/*
		 * Check to see if the bootline is defined in the 'bootargs'
		 * parameter. If it is not defined, we may be able to
		 * construct the info.
		 */
		bootline = getenv("bootargs");
		if (bootline) {
			memcpy((void *)bootaddr, bootline,
			       max(strlen(bootline), (size_t)255));
			flush_cache(bootaddr, max(strlen(bootline),
						  (size_t)255));
		} else {
			tmp = getenv("bootdev");
			if (tmp)
				ptr = sprintf(build_buf, tmp);
			else
				printf("## VxWorks boot device not specified\n");

			tmp = getenv("bootfile");
			if (tmp)
				ptr += sprintf(build_buf + ptr,
					       "host:%s ", tmp);
			else
				ptr += sprintf(build_buf + ptr,
					       "host:vxWorks ");

			/*
			 * The following parameters are only needed if 'bootdev'
			 * is an ethernet device, otherwise they are optional.
			 */
			tmp = getenv("ipaddr");
			if (tmp) {
				ptr += sprintf(build_buf + ptr, "e=%s", tmp);
				tmp = getenv("netmask");
				if (tmp) {
					u32 mask = getenv_ip("netmask").s_addr;
					ptr += sprintf(build_buf + ptr,
						       ":%08x ", ntohl(mask));
				} else {
					ptr += sprintf(build_buf + ptr, " ");
				}
			}

			tmp = getenv("serverip");
			if (tmp)
				ptr += sprintf(build_buf + ptr, "h=%s ", tmp);

			tmp = getenv("gatewayip");
			if (tmp)
				ptr += sprintf(build_buf + ptr, "g=%s ", tmp);

			tmp = getenv("hostname");
			if (tmp)
				ptr += sprintf(build_buf + ptr, "tn=%s ", tmp);

			tmp = getenv("othbootargs");
			if (tmp)
				ptr += sprintf(build_buf + ptr, tmp);

			memcpy((void *)bootaddr, build_buf,
			       max(strlen(build_buf), (size_t)255));
			flush_cache(bootaddr, max(strlen(build_buf),
						  (size_t)255));
		}

		printf("## Using bootline (@ 0x%lx): %s\n", bootaddr,
		       (char *)bootaddr);
	}

#ifdef CONFIG_X86
	/*
	 * Since E820 information is critical to the kernel, if we don't
	 * specify these in the environments, use a default one.
	 */
	tmp = getenv("e820data");
	if (tmp)
		data = (struct e820entry *)simple_strtoul(tmp, NULL, 16);
	else
		data = (struct e820entry *)VXWORKS_E820_DATA_ADDR;
	tmp = getenv("e820info");
	if (tmp)
		info = (struct e820info *)simple_strtoul(tmp, NULL, 16);
	else
		info = (struct e820info *)VXWORKS_E820_INFO_ADDR;

	memset(info, 0, sizeof(struct e820info));
	info->sign = E820_SIGNATURE;
	info->entries = install_e820_map(E820MAX, data);
	info->addr = (info->entries - 1) * sizeof(struct e820entry) +
		     VXWORKS_E820_DATA_ADDR;
#endif

	/*
	 * If the data at the load address is an elf image, then
	 * treat it like an elf image. Otherwise, assume that it is a
	 * binary image.
	 */
	if (valid_elf_image(addr))
		addr = load_elf_image_shdr(addr);
	else
		puts("## Not an ELF image, assuming binary\n");

	printf("## Starting vxWorks at 0x%08lx ...\n", addr);

	dcache_disable();
#ifdef CONFIG_X86
	/* VxWorks on x86 uses stack to pass parameters */
	((asmlinkage void (*)(int))addr)(0);
#else
	((void (*)(int))addr)(0);
#endif

	puts("## vxWorks terminated\n");

	return 1;
}

U_BOOT_CMD(
	bootelf, 3, 0, do_bootelf,
	"Boot from an ELF image in memory",
	"[-p|-s|-u] [address]\n"
	"\t- load ELF image at [address] via program headers (-p)\n"
	"\t- load ELF image at [address] via section headers (-s)\n"
	"\t- validate and transfer to another ARM U-Boot (-u)"
);

U_BOOT_CMD(
	bootvx, 2, 0, do_bootvx,
	"Boot vxWorks from an ELF image",
	" [address] - load address of vxWorks ELF image."
);
