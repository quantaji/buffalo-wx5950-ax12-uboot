/*
 * Buffalo WXR-5950AX12 GPIO, PHY reset, USB VBUS and
 * U-Boot handoff preparation.
 */

#include <common.h>
#include <fdtdec.h>
#include <asm/armv7.h>
#include <asm/errno.h>
#include <asm/gpio.h>
#include <asm/io.h>

#include "ipq807x.h"

#define WXR_PCIE_PARF_PHY_CTRL		0x40
#define WXR_PCIE_PARF_HANDOFF_STATE	0x358

#define WXR_PCIE_PHY_READY		0x00000900
#define WXR_PCIE0_HANDOFF_READY		0x01000000
#define WXR_PCIE1_HANDOFF_READY		0x10000000

DECLARE_GLOBAL_DATA_PTR;

struct wxr_gpio_name {
	const char *name;
	const char *property;
};

static const struct wxr_gpio_name wxr_gpio_names[] = {
	{ "mode-ap", "mode_ap" },
	{ "mode-wb", "mode_wb" },
	{ "op-manual", "op_manual" },
	{ "wps", "wps" },
	{ "reset", "reset" },
	{ "led-router-white", "led_router_white" },
	{ "led-router-red", "led_router_red" },
	{ "led-power-red", "led_power_red" },
	{ "led-power-white", "led_power_white" },
	{ "led-internet-white", "led_internet_white" },
	{ "led-internet-red", "led_internet_red" },
	{ "led-wlan-red", "led_wlan_red" },
	{ "led-wlan-white", "led_wlan_white" },
	{ "usb-vbus", "usb_vbus" },
	{ "qca8075-reset", "qca8075_reset" },
	{ "aqr113c-reset", "aqr113c_reset" },
};

static int wxr5950ax12_get_gpio(const char *property)
{
	int gpio;
	int node;

	node = fdt_path_offset(gd->fdt_blob, "/board-gpio");
	if (node < 0)
		return node;

	gpio = fdtdec_get_uint(gd->fdt_blob, node, property, -1);
	if (gpio < 0 || gpio > 89)
		return -EINVAL;

	return gpio;
}

static int wxr5950ax12_get_pcie_parf(const char *path,
				     struct fdt_resource *parf)
{
	int node;

	node = fdt_path_offset(gd->fdt_blob, path);
	if (node < 0)
		return node;

	return fdt_get_named_resource(gd->fdt_blob, node,
				      "reg", "reg-names", "parf", parf);
}

int board_uboot_handoff_prepare(void)
{
	struct fdt_resource pcie0_parf;
	struct fdt_resource pcie1_parf;
	uint32_t soc_ver_major;
	uint32_t soc_ver_minor;
	uint32_t sys_noc0;
	uint32_t sys_noc1;
	int deinitialized;

	get_soc_version(&soc_ver_major, &soc_ver_minor);
	if (soc_ver_major != 2) {
		printf("WXR U-Boot handoff: unsupported SoC version %u.%u\n",
		       soc_ver_major, soc_ver_minor);
		return -EINVAL;
	}

	if (wxr5950ax12_get_pcie_parf("/pci@20000000", &pcie0_parf) ||
	    wxr5950ax12_get_pcie_parf("/pci@10000000", &pcie1_parf)) {
		puts("WXR U-Boot handoff: PCIe PARF resources are missing\n");
		return -ENOENT;
	}

	sys_noc0 = readl(GCC_SYS_NOC_PCIE0_AXI_CLK) & 1;
	sys_noc1 = readl(GCC_SYS_NOC_PCIE1_AXI_CLK) & 1;

	if (sys_noc0 && sys_noc1) {
		if (readl(pcie0_parf.start + WXR_PCIE_PARF_PHY_CTRL) !=
							WXR_PCIE_PHY_READY ||
		    readl(pcie1_parf.start + WXR_PCIE_PARF_PHY_CTRL) !=
							WXR_PCIE_PHY_READY ||
		    readl(pcie0_parf.start + WXR_PCIE_PARF_HANDOFF_STATE) !=
							WXR_PCIE0_HANDOFF_READY ||
		    readl(pcie1_parf.start + WXR_PCIE_PARF_HANDOFF_STATE) !=
							WXR_PCIE1_HANDOFF_READY) {
			puts("WXR U-Boot handoff: unknown active PCIe state\n");
			return -EIO;
		}

		return 0;
	}

	deinitialized = !sys_noc0 && !sys_noc1;
	if (!deinitialized) {
		puts("WXR U-Boot handoff: inconsistent SYS_NOC state\n");
		return -EIO;
	}

	writel(1, GCC_SYS_NOC_PCIE0_AXI_CLK);
	writel(1, GCC_SYS_NOC_PCIE1_AXI_CLK);
	ipq807x_pcie_v2_clock_init(0);

	writel(WXR_PCIE_PHY_READY,
	       pcie0_parf.start + WXR_PCIE_PARF_PHY_CTRL);
	writel(WXR_PCIE_PHY_READY,
	       pcie1_parf.start + WXR_PCIE_PARF_PHY_CTRL);
	writel(WXR_PCIE0_HANDOFF_READY,
	       pcie0_parf.start + WXR_PCIE_PARF_HANDOFF_STATE);
	writel(WXR_PCIE1_HANDOFF_READY,
	       pcie1_parf.start + WXR_PCIE_PARF_HANDOFF_STATE);

	DSB;

	if (!(readl(GCC_SYS_NOC_PCIE0_AXI_CLK) & 1) ||
	    !(readl(GCC_SYS_NOC_PCIE1_AXI_CLK) & 1) ||
	    readl(pcie0_parf.start + WXR_PCIE_PARF_PHY_CTRL) !=
							WXR_PCIE_PHY_READY ||
	    readl(pcie1_parf.start + WXR_PCIE_PARF_PHY_CTRL) !=
							WXR_PCIE_PHY_READY ||
	    readl(pcie0_parf.start + WXR_PCIE_PARF_HANDOFF_STATE) !=
							WXR_PCIE0_HANDOFF_READY ||
	    readl(pcie1_parf.start + WXR_PCIE_PARF_HANDOFF_STATE) !=
							WXR_PCIE1_HANDOFF_READY) {
		puts("WXR U-Boot handoff: PCIe state restoration failed\n");
		return -EIO;
	}

	return 0;
}

int name_to_gpio(const char *name)
{
	char *end;
	unsigned long gpio;
	int i;

	if (!name || !*name)
		return -EINVAL;

	gpio = simple_strtoul(name, &end, 10);
	if (*end == '\0')
		return gpio <= 89 ? gpio : -EINVAL;

	for (i = 0; i < ARRAY_SIZE(wxr_gpio_names); i++) {
		if (!strcmp(name, wxr_gpio_names[i].name))
			return wxr5950ax12_get_gpio(wxr_gpio_names[i].property);
	}

	return -ENOENT;
}

static int phy_init_done;

int wxr5950ax12_prepare_phy_init(void)
{
	int aqr_reset;
	int qca_reset;

	if (phy_init_done)
		return 0;

#ifdef CONFIG_BUFFALO_WXR5950AX12_RAM_UBOOT
	puts("WXR PHY init: preserving PHY reset state for RAM takeover\n");
	phy_init_done = 1;
	return 0;
#endif

	qca_reset = wxr5950ax12_get_gpio("qca8075_reset");
	aqr_reset = wxr5950ax12_get_gpio("aqr113c_reset");
	if (qca_reset < 0 || aqr_reset < 0) {
		puts("WXR PHY init: reset GPIO description is invalid\n");
		return -EINVAL;
	}

	printf("WXR PHY init: cold reset on GPIO%d and GPIO%d\n",
	       qca_reset, aqr_reset);
	gpio_direction_output(qca_reset, 0);
	gpio_direction_output(aqr_reset, 0);
	mdelay(500);
	gpio_set_value(qca_reset, 1);
	gpio_set_value(aqr_reset, 1);
	mdelay(500);
	phy_init_done = 1;

	return 0;
}

int wxr5950ax12_usb_vbus_enable(void)
{
	int gpio = wxr5950ax12_get_gpio("usb_vbus");

	if (gpio < 0) {
		puts("WXR USB: VBUS GPIO description is invalid\n");
		return gpio;
	}

	return gpio_direction_output(gpio, 1);
}
