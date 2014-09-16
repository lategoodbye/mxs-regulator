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
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

#define HW_POWER_STS	        (0x000000c0)

#define BM_POWER_STS_DC_OK	(1 << 9)
#define BM_POWER_REG_MODE       (1 << 17)

struct mxs_regulator {
	struct regulator_desc rdesc;
	struct regulator_init_data *initdata;

	spinlock_t lock;
	struct notifier_block nb;

	const char *name;
	void __iomem *base_addr;
	void __iomem *power_addr;
	int mode;
};

static int mxs_set_voltage(struct regulator_dev *reg, int min_uV, int max_uV,
			   unsigned *selector)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulation_constraints *con = &sreg->initdata->constraints;
	void __iomem *power_sts = sreg->power_addr + HW_POWER_STS;
	u32 val, regs, i;

	dev_dbg(&reg->dev, "%s: uv %d, min %d, max %d\n", __func__, max_uV,
			   con->min_uV, con->max_uV);

	if (max_uV < con->min_uV || max_uV > con->max_uV)
		return -EINVAL;

	val = (max_uV - con->min_uV) * sreg->rdesc.n_voltages /
			(con->max_uV - con->min_uV);

	regs = (readl(sreg->base_addr) & ~sreg->rdesc.vsel_mask);

	dev_dbg(&reg->dev, "%s: calculated val %d\n", __func__, val);

	writel(val | regs, sreg->base_addr);
	for (i = 20; i; i--) {
		if (readl(power_sts) & BM_POWER_STS_DC_OK)
			return 0;

		udelay(1);
	}

	writel(val | regs, sreg->base_addr);
	for (i = 40000; i; i--) {
		if (readl(power_sts) & BM_POWER_STS_DC_OK)
			return 0;

		udelay(1);
	}

	for (i = 40000; i; i--) {
		if (readl(power_sts) & BM_POWER_STS_DC_OK)
			return 0;

		udelay(1);
	}

	/* Give up */
	return -ETIMEDOUT;
}


static int mxs_get_voltage(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct regulation_constraints *con = &sreg->initdata->constraints;
	int uv;
	u32 val = readl(sreg->base_addr) & sreg->rdesc.vsel_mask;

	if (val > sreg->rdesc.n_voltages)
		val = sreg->rdesc.n_voltages;

	uv = con->min_uV + val *
		(con->max_uV - con->min_uV) / sreg->rdesc.n_voltages;

	return uv;
}

static int mxs_set_mode(struct regulator_dev *reg, unsigned int mode)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	int ret = 0;
	u32 val;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = readl(sreg->base_addr);
		writel(val | BM_POWER_REG_MODE, sreg->base_addr);
		break;

	case REGULATOR_MODE_NORMAL:
		val = readl(sreg->base_addr);
		writel(val & ~BM_POWER_REG_MODE, sreg->base_addr);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static unsigned int mxs_get_mode(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	u32 val = readl(sreg->base_addr) & BM_POWER_REG_MODE;

	return val ? REGULATOR_MODE_FAST : REGULATOR_MODE_NORMAL;
}

static struct regulator_ops mxs_vol_rops = {
	.set_voltage	= mxs_set_voltage,
	.get_voltage	= mxs_get_voltage,
	.set_mode	= mxs_set_mode,
	.get_mode	= mxs_get_mode,
};

#define MXS_VDDIO	1
#define MXS_VDDA	2
#define MXS_VDDD	3

static struct regulator_desc mxs_reg_desc[] = {
	{
		.name = "vddio",
		.id = MXS_VDDIO,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x10,
		.uV_step = 50000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
	},
	{
		.name = "vdda",
		.id = MXS_VDDA,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
	},
	{
		.name = "vddd",
		.id = MXS_VDDD,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
	},
};

static int mxs_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *parent;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct mxs_regulator *sreg;
	struct regulator_init_data *initdata;
	struct regulation_constraints *con;
	struct regulator_config config = { };
	void __iomem *base_addr = NULL;
	void __iomem *power_addr = NULL;
	u64 regaddr64 = 0;
	const u32 *regaddr_p;
	int ret = 0;
	const char *name;
	int i;

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	initdata = of_get_regulator_init_data(dev, np);
	if (!initdata)
		return -EINVAL;

	if (of_property_read_string(np, "regulator-name", &name))
		return -EINVAL;

	sreg = devm_kzalloc(dev, sizeof(*sreg), GFP_KERNEL);
	if (!sreg)
		return -EINVAL;

	sreg->initdata = initdata;
	rdesc = &sreg->rdesc;
	memset(rdesc, 0, sizeof(*rdesc));

	for (i = 0; i < ARRAY_SIZE(mxs_reg_desc); i++) {
		if (!strcmp(mxs_reg_desc[i].name, name)) {
			sreg->name = name;
			rdesc->supply_name    = mxs_reg_desc[i].supply_name;
			rdesc->id	      = mxs_reg_desc[i].id;
			rdesc->type	      = mxs_reg_desc[i].type;
			rdesc->n_voltages     = mxs_reg_desc[i].n_voltages;
			rdesc->uV_step	      = mxs_reg_desc[i].uV_step;
			rdesc->linear_min_sel = mxs_reg_desc[i].linear_min_sel;
			rdesc->vsel_mask      = mxs_reg_desc[i].vsel_mask;
			break;
		}
	}

	if (i >= ARRAY_SIZE(mxs_reg_desc)) {
		dev_err(dev, "unknown regulator %s\n", name);
		return -EINVAL;
	}

	/* get device base address */
	base_addr = of_iomap(np, 0);
	if (!base_addr)
		return -ENXIO;

	parent = of_get_parent(np);
	if (!parent) {
		ret = -ENXIO;
		goto fail2;
	}

	/* get base address of power controller */
	power_addr = of_iomap(parent, 0);
	of_node_put(parent);
	if (!power_addr) {
		ret = -ENXIO;
		goto fail2;
	}

	regaddr_p = of_get_address(np, 0, NULL, NULL);
	if (regaddr_p)
		regaddr64 = of_translate_address(np, regaddr_p);

	if (!regaddr64) {
		dev_err(dev, "no or invalid reg property set\n");
		ret = -EINVAL;
		goto fail3;
	}

	dev_info(dev, "regulator %s found\n", name);

	sreg->base_addr = base_addr;
	sreg->power_addr = power_addr;
	spin_lock_init(&sreg->lock);

	rdesc->name = sreg->name;
	rdesc->owner = THIS_MODULE;
	rdesc->ops = &mxs_vol_rops;

	con = &initdata->constraints;
	rdesc->min_uV = con->min_uV;
	rdesc->vsel_reg = regaddr64;

	config.dev = &pdev->dev;
	config.init_data = initdata;
	config.driver_data = sreg;
	config.of_node = np;

	dev_dbg(dev, "probing regulator %s %d\n", name, pdev->id);

	rdev = devm_regulator_register(dev, rdesc, &config);

	if (IS_ERR(rdev)) {
		dev_err(dev, "failed to register %s\n", name);
		ret = PTR_ERR(rdev);
		goto fail3;
	}

	platform_set_drvdata(pdev, rdev);

	return 0;
fail3:
	iounmap(power_addr);
fail2:
	iounmap(base_addr);

	return ret;
}

static int mxs_regulator_remove(struct platform_device *pdev)
{
	struct regulator_dev *rdev = platform_get_drvdata(pdev);
	struct mxs_regulator *sreg = rdev_get_drvdata(rdev);
	void __iomem *base_addr = sreg->base_addr;
	void __iomem *power_addr = sreg->power_addr;

	regulator_unregister(rdev);
	iounmap(power_addr);
	iounmap(base_addr);

	return 0;
}

static struct of_device_id of_mxs_regulator_match_tbl[] = {
	{ .compatible = "fsl,mxs-regulator", },
	{ /* end */ }
};

static struct platform_driver mxs_regulator_driver = {
	.driver = {
		.name	= "mxs-regulator",
		.owner  = THIS_MODULE,
		.of_match_table = of_mxs_regulator_match_tbl,
	},
	.probe	= mxs_regulator_probe,
	.remove = mxs_regulator_remove,
};

static int __init mxs_regulator_init(void)
{
	return platform_driver_register(&mxs_regulator_driver);
}
postcore_initcall(mxs_regulator_init);

static void __exit mxs_regulator_exit(void)
{
	platform_driver_unregister(&mxs_regulator_driver);
}
module_exit(mxs_regulator_exit);

MODULE_AUTHOR("Embedded Alley Solutions <source@embeddedalley.com>");
MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_DESCRIPTION("Freescale STMP378X voltage regulators");
MODULE_LICENSE("GPL v2");
