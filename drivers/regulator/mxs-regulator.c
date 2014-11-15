/*
 * Freescale STMP378X voltage regulators
 *
 * Embedded Alley Solutions, Inc <source@embeddedalley.com>
 *
 * Copyright (C) 2014 Stefan Wahren
 * Copyright (C) 2010 Freescale Semiconductor, Inc.
 * Copyright 2008 Embedded Alley Solutions, Inc All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

#define HW_POWER_LINREG_OFFSET_LINREG_MODE	0
#define HW_POWER_LINREG_OFFSET_DCDC_MODE	2

/* Powered by linear regulator.  DCDC output is gated off and
   the linreg output is equal to the target. */
#define HW_POWER_LINREG_DCDC_OFF		1

/* Powered by linear regulator.  DCDC output is not gated off
   and is ready for the automatic hardware transistion after a 5V
   event.  The converters are not enabled when 5V is present. LinReg output
   is 25mV below target. */
#define HW_POWER_LINREG_DCDC_READY		2

/* Powered by DCDC converter and the LinReg is on. LinReg output
   is 25mV below target. */
#define HW_POWER_DCDC_LINREG_ON			3

/* Powered by DCDC converter and the LinReg is off. LinReg output
   is 25mV below target. */
#define HW_POWER_DCDC_LINREG_OFF		4

/* Powered by DCDC converter and the LinReg is ready for the
   automatic hardware transfer.  The LinReg output is not enabled and
   depends on the 5V presence to enable the LinRegs.  LinReg offset is 25mV
   below target. */
#define HW_POWER_DCDC_LINREG_READY		5

/* Powered by an external source when 5V is present.  This does not
   necessarily mean the external source is powered by 5V,but the chip needs
   to be aware that 5V is present. */
#define HW_POWER_EXTERNAL_SOURCE_5V		6

/* Powered by an external source when 5V is not present.This doesn't
   necessarily mean the external source is powered by the battery, but the
   chip needs to be aware that the battery is present */
#define HW_POWER_EXTERNAL_SOURCE_BATTERY	7

/* Unknown configuration.  This is an error. */
#define HW_POWER_UNKNOWN_SOURCE			8

#define BM_POWER_STS_VBUSVALID0_STATUS	BIT(15)
#define BM_POWER_STS_DC_OK		BIT(9)

#define BM_POWER_5VCTRL_ILIMIT_EQ_ZERO	BIT(2)
#define BM_POWER_5VCTRL_ENABLE_DCDC	BIT(0)

#define MXS_VDDIO	1
#define MXS_VDDA	2
#define MXS_VDDD	3

struct mxs_regulator {
	struct regulator_desc desc;
	unsigned int disable_fet_mask;
	unsigned int linreg_offset_mask;
	u8 linreg_offset_shift;
	u8 (*get_power_source)(struct regulator_dev *);

	void __iomem *base_addr;
	void __iomem *status_addr;
	void __iomem *v5ctrl_addr;
};

void _decode_hw_power_5vctrl(u32 value)
{
	pr_info("HW_POWER_5VCTRL\n");
	pr_info("VBUSDROOP_TRSH: %x\n", (value >> 28) & 3);
	pr_info("HEADROOM_ADJ: %x\n", (value >> 24) & 7);
	pr_info("PWD_CHARGE_4P2: %x\n", (value >> 20) & 3);
	pr_info("CHARGE_4P2_ILIMIT: %x\n", (value >> 12) & 0x3F);
	pr_info("VBUSVALID_TRSH: %x\n", (value >> 8) & 7);
	pr_info("PWDN_5VBRNOUT: %x\n", (value >> 7) & 1);
	pr_info("ENABLE_LINREG_ILIMIT: %x\n", (value >> 6) & 1);
	pr_info("DCDC_XFER: %x\n", (value >> 5) & 1);
	pr_info("VBUSVALID_5VDETECT: %x\n", (value >> 4) & 1);
	pr_info("VBUSVALID_TO_B: %x\n", (value >> 3) & 1);
	pr_info("ILIMIT_EQ_ZERO: %x\n", (value >> 2) & 1);
	pr_info("PWRUP_VBUS_CMPS: %x\n", (value >> 1) & 1);
	pr_info("ENABLE_DCDC: %x\n", value & 1);
}

void _decode_hw_power_vddactrl(u32 value)
{
	pr_info("HW_POWER_VDDACTRL\n");
	pr_info("PWDN_BRNOUT: %x\n", (value >> 19) & 1);
	pr_info("DISABLE_STEPPING: %x\n", (value >> 18) & 1);
	pr_info("ENABLE_LINREG: %x\n", (value >> 17) & 1);
	pr_info("DISABLE_FET: %x\n", (value >> 16) & 1);
	pr_info("LINREG_OFFSET: %x\n", (value >> 12) & 3);
	pr_info("BO_OFFSET: %x\n", (value >> 8) & 7);
	pr_info("TRG: %x\n", value & 0x1f);
}

void _decode_hw_power_vdddctrl(u32 value)
{
	pr_info("HW_POWER_VDDDCTRL\n");
	pr_info("ADJTN: %x\n", (value >> 28) & 0xf);
	pr_info("PWDN_BRNOUT: %x\n", (value >> 23) & 1);
	pr_info("DISABLE_STEPPING: %x\n", (value >> 22) & 1);
	pr_info("ENABLE_LINREG: %x\n", (value >> 21) & 1);
	pr_info("DISABLE_FET: %x\n", (value >> 20) & 1);
	pr_info("LINREG_OFFSET: %x\n", (value >> 16) & 3);
	pr_info("BO_OFFSET: %x\n", (value >> 8) & 7);
	pr_info("TRG: %x\n", value & 0x1f);
}

void _decode_hw_power_vddioctrl(u32 value)
{
	pr_info("HW_POWER_VDDIOCTRL\n");
	pr_info("ADJTN: %x\n", (value >> 20) & 0xf);
	pr_info("PWDN_BRNOUT: %x\n", (value >> 18) & 1);
	pr_info("DISABLE_STEPPING: %x\n", (value >> 17) & 1);
	pr_info("DISABLE_FET: %x\n", (value >> 16) & 1);
	pr_info("LINREG_OFFSET: %x\n", (value >> 12) & 3);
	pr_info("BO_OFFSET: %x\n", (value >> 8) & 7);
	pr_info("TRG: %x\n", value & 0x1f);
}

static inline u8 get_linreg_offset(struct mxs_regulator *sreg, u32 regs)
{
	return regs & sreg->linreg_offset_mask >> sreg->linreg_offset_shift;
}

static u8 get_vddio_power_source(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	u32 v5ctrl, status, base;
	u8 linreg;

	v5ctrl = readl(sreg->v5ctrl_addr);
	status = readl(sreg->status_addr);
	base = readl(sreg->base_addr);
	linreg = get_linreg_offset(sreg, base);

	if (status & BM_POWER_STS_VBUSVALID0_STATUS) {
		if ((base & sreg->disable_fet_mask) &&
		    (linreg == HW_POWER_LINREG_OFFSET_LINREG_MODE)) {
			return HW_POWER_LINREG_DCDC_OFF;
		}

		if (v5ctrl & BM_POWER_5VCTRL_ENABLE_DCDC) {
			if (linreg == HW_POWER_LINREG_OFFSET_DCDC_MODE)
				return HW_POWER_DCDC_LINREG_ON;
		} else {
			if (linreg == HW_POWER_LINREG_OFFSET_LINREG_MODE)
				return HW_POWER_LINREG_DCDC_OFF;
		}
	} else {
		if (linreg == HW_POWER_LINREG_OFFSET_DCDC_MODE)
			return HW_POWER_DCDC_LINREG_ON;
	}

	return HW_POWER_UNKNOWN_SOURCE;
}

static u8 get_vdda_vddd_power_source(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulator_desc *desc = &sreg->desc;
	u32 v5ctrl, status, base;
	u8 linreg;

	v5ctrl = readl(sreg->v5ctrl_addr);
	status = readl(sreg->status_addr);
	base = readl(sreg->base_addr);
	linreg = get_linreg_offset(sreg, base);

	if (base & sreg->disable_fet_mask) {
		if (status & BM_POWER_STS_VBUSVALID0_STATUS)
			return HW_POWER_EXTERNAL_SOURCE_5V;

		if (linreg == HW_POWER_LINREG_OFFSET_LINREG_MODE)
			return HW_POWER_LINREG_DCDC_OFF;
	}

	if (status & BM_POWER_STS_VBUSVALID0_STATUS) {
		if (v5ctrl & BM_POWER_5VCTRL_ENABLE_DCDC)
			return HW_POWER_DCDC_LINREG_ON;

		return HW_POWER_LINREG_DCDC_OFF;
	}

	if (linreg == HW_POWER_LINREG_OFFSET_DCDC_MODE) {
		if (base & desc->enable_mask)
			return HW_POWER_DCDC_LINREG_ON;

		return HW_POWER_DCDC_LINREG_OFF;
	}

	return HW_POWER_UNKNOWN_SOURCE;
}

void print_power_source(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulator_desc *desc = &sreg->desc;
	u8 power_source = HW_POWER_UNKNOWN_SOURCE;

	if (sreg->get_power_source)
		power_source = sreg->get_power_source(reg);

	switch (power_source) {
	case HW_POWER_LINREG_DCDC_OFF:
		pr_info("%s: POWER SOURCE: LINREG (DCDC OFF)\n", desc->name);
		break;
	case HW_POWER_LINREG_DCDC_READY:
		pr_info("%s: POWER SOURCE: LINREG (DCDC READY)\n", desc->name);
		break;
	case HW_POWER_DCDC_LINREG_ON:
		pr_info("%s: POWER SOURCE: DCDC (LINREG ON)\n", desc->name);
		break;
	case HW_POWER_DCDC_LINREG_OFF:
		pr_info("%s: POWER SOURCE: DCDC (LINREG OFF)\n", desc->name);
		break;
	case HW_POWER_DCDC_LINREG_READY:
		pr_info("%s: POWER SOURCE: DCDC (LINREG READY)\n", desc->name);
		break;
	case HW_POWER_EXTERNAL_SOURCE_5V:
		pr_info("%s: POWER SOURCE: EXT SOURCE 5V\n", desc->name);
		break;
	case HW_POWER_EXTERNAL_SOURCE_BATTERY:
		pr_info("%s: POWER SOURCE: BATTERY\n", desc->name);
		break;
	default:
		pr_info("%s: POWER SOURCE: UNKNOWN\n", desc->name);
		break;
	}
}

static int mxs_set_voltage_sel(struct regulator_dev *reg, unsigned sel)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulator_desc *desc = &sreg->desc;
	unsigned long start;
	u32 regs;
	int uV;
	u8 power_source = HW_POWER_UNKNOWN_SOURCE;

	if (sel >= desc->n_voltages) {
		dev_err(&reg->dev, "%s: sel(%d) >= n_voltages(%d)\n", __func__,
			sel, desc->n_voltages);
		return -EINVAL;
	}

	uV = regulator_list_voltage_linear(reg, sel);

	if (uV >= 0)
		pr_debug("%s: %s: %d mV\n", __func__, desc->name, uV / 1000);

	regs = (readl(sreg->base_addr) & ~desc->vsel_mask);
	writel(sel | regs, sreg->base_addr);

	if (sreg->get_power_source)
		power_source = sreg->get_power_source(reg);

	switch (power_source) {
	case HW_POWER_LINREG_DCDC_OFF:
	case HW_POWER_LINREG_DCDC_READY:
	case HW_POWER_EXTERNAL_SOURCE_5V:
		msleep(1000);
		return 0;
	}

	usleep_range(15000, 20000);
	start = jiffies;
	while (1) {
		if (readl(sreg->status_addr) & BM_POWER_STS_DC_OK)
			return 0;

		if (time_after(jiffies, start +	msecs_to_jiffies(20000)))
			break;

		schedule();
	}

	dev_warn_ratelimited(&reg->dev, "%s: timeout status=0x%08x\n",
				  __func__, readl(sreg->status_addr));

	return -ETIMEDOUT;
}

static int mxs_get_voltage_sel(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulator_desc *desc = &sreg->desc;
	int ret, uV;

	ret = readl(sreg->base_addr) & desc->vsel_mask;
	uV = regulator_list_voltage_linear(reg, ret);

	if (uV >= 0)
		pr_debug("%s: %s: %d mV\n", __func__, desc->name, uV / 1000);

	return ret;
}

static struct regulator_ops mxs_rops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.set_voltage_sel	= mxs_set_voltage_sel,
	.get_voltage_sel	= mxs_get_voltage_sel,
};

static const struct mxs_regulator imx23_info_vddio = {
	.desc = {
		.name = "vddio",
		.id = MXS_VDDIO,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 0x20,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.min_uV = 2800000,
		.vsel_mask = 0x1f,
		.ops = &mxs_rops,
	},
	.disable_fet_mask = 1 << 16,
	.linreg_offset_mask = 3 << 12,
	.linreg_offset_shift = 12,
	.get_power_source = get_vddio_power_source,
};

static const struct mxs_regulator imx28_info_vddio = {
	.desc = {
		.name = "vddio",
		.id = MXS_VDDIO,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 0x11,
		.uV_step = 50000,
		.linear_min_sel = 0,
		.min_uV = 2800000,
		.vsel_mask = 0x1f,
		.ops = &mxs_rops,
	},
	.disable_fet_mask = 1 << 16,
	.linreg_offset_mask = 3 << 12,
	.linreg_offset_shift = 12,
	.get_power_source = get_vddio_power_source,
};

static const struct mxs_regulator mxs_info_vdda = {
	.desc = {
		.name = "vdda",
		.id = MXS_VDDA,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 0x20,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.min_uV = 1500000,
		.vsel_mask = 0x1f,
		.ops = &mxs_rops,
		.enable_mask = (1 << 17),
	},
	.disable_fet_mask = 1 << 16,
	.linreg_offset_mask = 3 << 12,
	.linreg_offset_shift = 12,
	.get_power_source = get_vdda_vddd_power_source,
};

static const struct mxs_regulator mxs_info_vddd = {
	.desc = {
		.name = "vddd",
		.id = MXS_VDDD,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 0x20,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.min_uV = 800000,
		.vsel_mask = 0x1f,
		.ops = &mxs_rops,
		.enable_mask = (1 << 21),
	},
	.disable_fet_mask = 1 << 20,
	.linreg_offset_mask = 3 << 16,
	.linreg_offset_shift = 16,
	.get_power_source = get_vdda_vddd_power_source,
};

static const struct of_device_id of_mxs_regulator_match[] = {
	{ .compatible = "fsl,imx23-vddio", .data = &imx23_info_vddio },
	{ .compatible = "fsl,imx23-vdda",  .data = &mxs_info_vdda },
	{ .compatible = "fsl,imx23-vddd",  .data = &mxs_info_vddd },
	{ .compatible = "fsl,imx28-vddio", .data = &imx28_info_vddio },
	{ .compatible = "fsl,imx28-vdda",  .data = &mxs_info_vdda },
	{ .compatible = "fsl,imx28-vddd",  .data = &mxs_info_vddd },
	{ /* end */ }
};
MODULE_DEVICE_TABLE(of, of_mxs_regulator_match);

static void regulator_init(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulator_desc *desc = &sreg->desc;
	u32 base = readl(sreg->base_addr);
	u8 linreg = get_linreg_offset(sreg, base);
	u8 power_source = HW_POWER_UNKNOWN_SOURCE;

	if (sreg->get_power_source)
		power_source = sreg->get_power_source(reg);

	/* According to AN4199 avoid possible LinReg and DC-DC contention */
	if (linreg < HW_POWER_LINREG_OFFSET_DCDC_MODE) {
		switch (power_source) {
		case HW_POWER_DCDC_LINREG_ON:
		case HW_POWER_DCDC_LINREG_READY:
		case HW_POWER_EXTERNAL_SOURCE_5V:
			base &= ~sreg->linreg_offset_mask;
			base |= HW_POWER_LINREG_OFFSET_DCDC_MODE;
			writel(base, sreg->base_addr);
			dev_info(&reg->dev, "%s: Set LinReg offset below DCDC target\n",
					    desc->name);
			break;
		}
	}
}

static int mxs_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	const struct mxs_regulator *template;
	struct device_node *np = dev->of_node;
	struct regulator_dev *rdev = NULL;
	struct mxs_regulator *sreg;
	struct regulator_init_data *initdata = NULL;
	struct regulator_config config = { };
	struct resource *res;
	int ret = 0;
	char *pname;

	match = of_match_device(of_mxs_regulator_match, dev);
	if (!match) {
		/* We do not expect this to happen */
		dev_err(dev, "%s: Unable to match device\n", __func__);
		return -ENODEV;
	}

	template = match->data;

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	initdata = of_get_regulator_init_data(dev, np);
	if (!initdata) {
		dev_err(dev, "missing regulator init data\n");
		return -EINVAL;
	}

	sreg = devm_kmemdup(&pdev->dev, template, sizeof(*sreg), GFP_KERNEL);
	if (!sreg)
		return -ENOMEM;

	pname = "base-address";
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, pname);
	if (!res) {
		dev_err(dev, "Missing '%s' IO resource\n", pname);
		return -ENODEV;
	}
	sreg->base_addr = devm_ioremap_nocache(dev, res->start,
						 resource_size(res));
	if (IS_ERR(sreg->base_addr))
		return PTR_ERR(sreg->base_addr);

	/* status register is shared between the regulators */
	pname = "status-address";
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, pname);
	if (!res) {
		dev_err(dev, "Missing '%s' IO resource\n", pname);
		return -ENODEV;
	}
	sreg->status_addr = devm_ioremap_nocache(dev, res->start,
						 resource_size(res));
	if (IS_ERR(sreg->status_addr))
		return PTR_ERR(sreg->status_addr);

	pname = "v5ctrl-address";
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, pname);
	if (!res) {
		dev_err(dev, "Missing '%s' IO resource\n", pname);
		return -ENODEV;
	}
	sreg->v5ctrl_addr = devm_ioremap_nocache(dev, res->start,
						 resource_size(res));
	if (IS_ERR(sreg->v5ctrl_addr))
		return PTR_ERR(sreg->v5ctrl_addr);

	config.dev = &pdev->dev;
	config.init_data = initdata;
	config.driver_data = sreg;
	config.of_node = np;

	rdev = devm_regulator_register(dev, &sreg->desc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		dev_err(dev, "%s: failed to register regulator(%d)\n",
			__func__, ret);
		return ret;
	}

	regulator_init(rdev);
	platform_set_drvdata(pdev, rdev);

	return 0;
}

static struct platform_driver mxs_regulator_driver = {
	.driver = {
		.name	= "mxs_regulator",
		.owner  = THIS_MODULE,
		.of_match_table = of_mxs_regulator_match,
	},
	.probe	= mxs_regulator_probe,
};

module_platform_driver(mxs_regulator_driver);

MODULE_AUTHOR("Embedded Alley Solutions <source@embeddedalley.com>");
MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_DESCRIPTION("Freescale STMP378X voltage regulators");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mxs_regulator");
