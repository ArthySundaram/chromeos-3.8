/* linux/drivers/usb/phy/samsung-usbphy.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com
 *
 * Author: Praveen Paneri <p.paneri@samsung.com>
 *
 * Samsung USB-PHY transceiver; talks to S3C HS OTG controller, EHCI-S5P and
 * OHCI-EXYNOS controllers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/usb/samsung_usb_phy.h>

#include "samsung-usbphy.h"

int samsung_usbphy_parse_dt(struct samsung_usbphy *sphy)
{
	struct device_node *usbphy_sys;
	int ret;

	/* Getting node for system controller interface for usb-phy */
	usbphy_sys = of_get_child_by_name(sphy->dev->of_node, "usbphy-sys");
	if (!usbphy_sys) {
		dev_err(sphy->dev, "No sys-controller interface for usb-phy\n");
		return -ENODEV;
	}

	sphy->pmuregs = of_iomap(usbphy_sys, 0);

	if (sphy->pmuregs == NULL) {
		dev_err(sphy->dev, "Can't get usb-phy pmu control register\n");
		goto err0;
	}

	sphy->sysreg = of_iomap(usbphy_sys, 1);

	/*
	 * Not returning error code here, since this situation is not fatal.
	 * Few SoCs may not have this switch available
	 */
	if (sphy->sysreg == NULL)
		dev_warn(sphy->dev, "Can't get usb-phy sysreg cfg register\n");

	/* Getting PHY clk gpio here to enable/disable PHY clock PLL, if any */
	sphy->phyclk_gpio = of_get_named_gpio(sphy->dev->of_node,
						"samsung,phyclk-gpio", 0);
	/*
	 * We don't want to return error code here in case we don't get the
	 * PHY clock gpio, some PHYs may not have it.
	 */
	if (gpio_is_valid(sphy->phyclk_gpio)) {
		ret = gpio_request_one(sphy->phyclk_gpio, GPIOF_INIT_HIGH,
						"samsung_usb_phy_clock_en");
		if (ret) {
			/*
			 * We don't want to return error code here,
			 * sometimes either of usb2 phy or usb3 phy may not
			 * have the PHY clock gpio.
			 */
			dev_err(sphy->dev, "can't request phyclk gpio %d\n",
						sphy->phyclk_gpio);
			sphy->phyclk_gpio = -EINVAL;
		}
	} else {
		dev_warn(sphy->dev, "Can't get usb-phy clock gpio\n");
	}

	of_node_put(usbphy_sys);

	return 0;

err0:
	of_node_put(usbphy_sys);
	return -ENXIO;
}

/*
 * Set isolation here for phy.
 * Here 'on = true' would mean USB PHY block is isolated, hence
 * de-activated and vice-versa.
 */
void samsung_usbphy_set_isolation(struct samsung_usbphy *sphy, bool on)
{
	void __iomem *reg = NULL;
	u32 reg_val;
	u32 en_mask = 0;

	if (!sphy->pmuregs) {
		dev_warn(sphy->dev, "Can't set pmu isolation\n");
		return;
	}

	switch (sphy->drv_data->cpu_type) {
	case TYPE_S3C64XX:
		/*
		 * Do nothing: We will add here once S3C64xx goes for DT support
		 */
		break;
	case TYPE_EXYNOS4210:
		/*
		 * Fall through since exynos4210 and exynos5250 have similar
		 * register architecture: two separate registers for host and
		 * device phy control with enable bit at position 0.
		 */
	case TYPE_EXYNOS5250:
		if (sphy->phy_type == USB_PHY_TYPE_DEVICE) {
			reg = sphy->pmuregs +
				sphy->drv_data->devphy_reg_offset;
			en_mask = sphy->drv_data->devphy_en_mask;
		} else if (sphy->phy_type == USB_PHY_TYPE_HOST) {
			reg = sphy->pmuregs +
				sphy->drv_data->hostphy_reg_offset;
			en_mask = sphy->drv_data->hostphy_en_mask;
		}
		break;
	default:
		dev_err(sphy->dev, "Invalid SoC type\n");
		return;
	}

	reg_val = readl(reg);

	if (on)
		reg_val &= ~en_mask;
	else
		reg_val |= en_mask;

	writel(reg_val, reg);
}

/*
 * Configure the mode of working of usb-phy here: HOST/DEVICE.
 */
void samsung_usbphy_cfg_sel(struct samsung_usbphy *sphy)
{
	u32 reg;

	if (!sphy->sysreg) {
		dev_warn(sphy->dev, "Can't configure specified phy mode\n");
		return;
	}

	reg = readl(sphy->sysreg);

	if (sphy->phy_type == USB_PHY_TYPE_DEVICE)
		reg &= ~EXYNOS_USB20PHY_CFG_HOST_LINK;
	else if (sphy->phy_type == USB_PHY_TYPE_HOST)
		reg |= EXYNOS_USB20PHY_CFG_HOST_LINK;

	writel(reg, sphy->sysreg);
}

/*
 * PHYs are different for USB Device and USB Host.
 * This make sure that correct PHY type is selected before
 * any operation on PHY.
 */
int samsung_usbphy_set_type(struct usb_phy *phy,
				enum samsung_usb_phy_type phy_type)
{
	struct samsung_usbphy *sphy = phy_to_sphy(phy);

	sphy->phy_type = phy_type;

	return 0;
}

/*
 * Returns reference clock frequency selection value
 */
int samsung_usbphy_get_refclk_freq(struct samsung_usbphy *sphy)
{
	struct clk *ref_clk;
	int refclk_freq = 0;

	/*
	 * In exynos5250 USB host and device PHY use
	 * external crystal clock XXTI
	 */
	if (sphy->drv_data->cpu_type == TYPE_EXYNOS5250)
		ref_clk = clk_get(sphy->dev, "ext_xtal");
	else
		ref_clk = clk_get(sphy->dev, "xusbxti");
	if (IS_ERR(ref_clk)) {
		dev_err(sphy->dev, "Failed to get reference clock\n");
		return PTR_ERR(ref_clk);
	}

	if (sphy->drv_data->cpu_type == TYPE_EXYNOS5250) {
		/* set clock frequency for PLL */
		switch (clk_get_rate(ref_clk)) {
		case 9600 * KHZ:
			refclk_freq = FSEL_CLKSEL_9600K;
			break;
		case 10 * MHZ:
			refclk_freq = FSEL_CLKSEL_10M;
			break;
		case 12 * MHZ:
			refclk_freq = FSEL_CLKSEL_12M;
			break;
		case 19200 * KHZ:
			refclk_freq = FSEL_CLKSEL_19200K;
			break;
		case 20 * MHZ:
			refclk_freq = FSEL_CLKSEL_20M;
			break;
		case 50 * MHZ:
			refclk_freq = FSEL_CLKSEL_50M;
			break;
		case 24 * MHZ:
		default:
			/* default reference clock */
			refclk_freq = FSEL_CLKSEL_24M;
			break;
		}
	} else {
		switch (clk_get_rate(ref_clk)) {
		case 12 * MHZ:
			refclk_freq = PHYCLK_CLKSEL_12M;
			break;
		case 24 * MHZ:
			refclk_freq = PHYCLK_CLKSEL_24M;
			break;
		case 48 * MHZ:
			refclk_freq = PHYCLK_CLKSEL_48M;
			break;
		default:
			if (sphy->drv_data->cpu_type == TYPE_S3C64XX)
				refclk_freq = PHYCLK_CLKSEL_48M;
			else
				refclk_freq = PHYCLK_CLKSEL_24M;
			break;
		}
	}
	clk_put(ref_clk);

	return refclk_freq;
}
