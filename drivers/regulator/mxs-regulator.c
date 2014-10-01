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

#define HW_POWER_STS			(0x000000c0)

#define BM_POWER_STS_DC_OK		BIT(9)

#define MXS_VDDIO	1
#define MXS_VDDA	2
#define MXS_VDDD	3

struct mxs_regulator {
	struct regulator_desc desc;
	struct regulator_init_data *initdata;

	const char *name;
	void __iomem *base_addr;
	void __iomem *status_addr;
};

static int mxs_set_voltage_sel(struct regulator_dev *reg, unsigned sel)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	unsigned long start;
	u32 regs;

	if (!sreg) {
		dev_err_ratelimited(&reg->dev, "%s: No regulator drvdata\n",
				    __func__);
		return -ENODEV;
	}

	pr_debug("%s: sel %u\n", __func__, sel);

	regs = (readl(sreg->base_addr) & ~sreg->desc.vsel_mask);

	writel(sel | regs, sreg->base_addr);
	start = jiffies;
	while (1) {
		if (readl(sreg->status_addr) & BM_POWER_STS_DC_OK)
			return 0;

		if (time_after(jiffies, start +	msecs_to_jiffies(80)))
			break;

		schedule();
	}

	return -ETIMEDOUT;
}

static int mxs_get_voltage_sel(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	int ret;

	if (!sreg) {
		dev_err_ratelimited(&reg->dev, "%s: No regulator drvdata\n",
				    __func__);
		return -ENODEV;
	}

	ret = readl(sreg->base_addr) & sreg->desc.vsel_mask;

	pr_debug("%s: sel %u\n", __func__, ret);

	return ret;
}

static struct regulator_ops mxs_rops = {
	.list_voltage		= regulator_list_voltage_linear,
	.set_voltage_sel	= mxs_set_voltage_sel,
	.get_voltage_sel	= mxs_get_voltage_sel,
};

static struct regulator_desc mxs_reg_desc[] = {
	{
		.name = "vddio",
		.id = MXS_VDDIO,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x10,
		.uV_step = 50000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
		.enable_mask = 0,
	},
	{
		.name = "vdda",
		.id = MXS_VDDA,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
		.enable_mask = (3 << 16),
	},
	{
		.name = "vddd",
		.id = MXS_VDDD,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
		.enable_mask = (3 << 20),
	},
};

static int mxs_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *parent;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev = NULL;
	struct mxs_regulator *sreg;
	struct regulator_init_data *initdata = NULL;
	struct regulation_constraints *con;
	struct regulator_config config = { };
	int ret = 0;
	struct resource *res;
	char *pname;
	const char *name;
	unsigned int i;

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	initdata = of_get_regulator_init_data(dev, np);
	if (!initdata) {
		dev_err(dev, "missing regulator init data\n");
		return -EINVAL;
	}

	if (of_property_read_string(np, "regulator-name", &name)) {
		dev_err(dev, "missing property regulator-name\n");
		return -EINVAL;
	}

	sreg = devm_kzalloc(dev, sizeof(*sreg), GFP_KERNEL);
	if (!sreg)
		return -EINVAL;

	sreg->initdata = initdata;
	rdesc = &sreg->desc;
	memset(rdesc, 0, sizeof(*rdesc));

	for (i = 0; i < ARRAY_SIZE(mxs_reg_desc); i++) {
		if (!strcmp(mxs_reg_desc[i].name, name))
			break;
	}

	if (i >= ARRAY_SIZE(mxs_reg_desc)) {
		dev_err(dev, "unknown regulator %s\n", name);
		return -EINVAL;
	}

	sreg->name = name;
	rdesc->name = sreg->name;
	rdesc->owner = THIS_MODULE;
	rdesc->id = mxs_reg_desc[i].id;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->linear_min_sel = mxs_reg_desc[i].linear_min_sel;
	rdesc->n_voltages = mxs_reg_desc[i].n_voltages;
	rdesc->uV_step = mxs_reg_desc[i].uV_step;
	rdesc->vsel_mask = mxs_reg_desc[i].vsel_mask;
	rdesc->enable_mask = mxs_reg_desc[i].enable_mask;
	rdesc->enable_is_inverted = mxs_reg_desc[i].enable_is_inverted;
	rdesc->ops = &mxs_rops;

	pname = "base-address";
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, pname);
	sreg->base_addr = devm_ioremap_resource(dev, res);
	if (IS_ERR(sreg->base_addr))
		return PTR_ERR(sreg->base_addr);

	/* status register is shared between the regulators */
	pname = "status-address";
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, pname);
	sreg->status_addr = devm_ioremap_nocache(dev, res->start,
						 resource_size(res));
	if (IS_ERR(sreg->status_addr))
		return PTR_ERR(sreg->status_addr);

	dev_info(dev, "%s found\n", name);

	sreg->base_addr = base_addr;
	sreg->power_addr = power_addr;

	con = &initdata->constraints;

	config.dev = &pdev->dev;
	config.init_data = initdata;
	config.driver_data = sreg;
	config.of_node = np;

	pr_debug("probing regulator %s\n", name);

	rdev = devm_regulator_register(dev, &sreg->desc, &config);
	if (IS_ERR(rdev)) {
		dev_err(dev, "failed to register %s\n", name);
		ret = PTR_ERR(rdev);
		goto fail2;
	}

	platform_set_drvdata(pdev, rdev);

	return 0;
fail2:
	iounmap(power_addr);
fail1:
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

static const struct of_device_id of_mxs_regulator_match[] = {
	{ .compatible = "fsl,mxs-regulator" },
	{ /* end */ }
};
MODULE_DEVICE_TABLE(of, of_mxs_regulator_match);

static struct platform_driver mxs_regulator_driver = {
	.driver = {
		.name	= "mxs_regulator",
		.owner  = THIS_MODULE,
		.of_match_table = of_mxs_regulator_match,
	},
	.probe	= mxs_regulator_probe,
	.remove = mxs_regulator_remove,
};

module_platform_driver(mxs_regulator_driver);

MODULE_AUTHOR("Embedded Alley Solutions <source@embeddedalley.com>");
MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_DESCRIPTION("Freescale STMP378X voltage regulators");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mxs_regulator");
