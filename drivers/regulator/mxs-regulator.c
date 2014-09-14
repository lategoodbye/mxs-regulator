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

#define HW_POWER_VDDDCTRL	(0x00000040)
#define HW_POWER_VDDACTRL	(0x00000050)
#define HW_POWER_VDDIOCTRL	(0x00000060)
#define HW_POWER_STS	        (0x000000c0)

#define BM_POWER_STS_DC_OK	(1 << 9)
#define BM_POWER_REG_MODE       (1 << 17)

#define MXS_REG5V_NOT_USB 0
#define MXS_REG5V_IS_USB 1


struct mxs_regulator {
	struct regulator_desc rdesc;
	struct regulator_init_data *initdata;
	struct mxs_regulator *parent;

	spinlock_t         lock;
	wait_queue_head_t  wait_q;
	struct notifier_block nb;

	const char *name;
	void __iomem *base_addr;
	void __iomem *power_addr;
	int mode;
	int cur_uV;
	int cur_uA;
	int max_uA;
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
			break;
		udelay(1);
	}

	if (i)
		goto out;

	writel(val | regs, sreg->base_addr);
	for (i = 40000; i; i--) {
		if (readl(power_sts) & BM_POWER_STS_DC_OK)
			break;
		udelay(1);
	}

	if (i)
		goto out;

	for (i = 40000; i; i--) {
		if (readl(power_sts) & BM_POWER_STS_DC_OK)
			break;
		udelay(1);
	}

out:
	return !i;
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

static int main_add_current(struct mxs_regulator *sreg,
			    int uA)
{
	pr_debug("%s: enter reg %s, uA=%d\n", __func__, sreg->name, uA);

	if (uA > 0 && (sreg->cur_uA + uA > sreg->max_uA))
		return -EINVAL;

	sreg->cur_uA += uA;

	return 0;
}

static int mxs_set_current_limit(struct regulator_dev *reg, int min_uA,
				 int max_uA)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);
	struct mxs_regulator *parent = sreg->parent;
	int ret = 0;
	unsigned long flags;

	dev_dbg(&reg->dev, "%s: enter reg %s, uA=%d\n", __func__, sreg->name,
			  max_uA);

	if (parent) {
		spin_lock_irqsave(&parent->lock, flags);
		ret = main_add_current(parent, max_uA - sreg->cur_uA);
		spin_unlock_irqrestore(&parent->lock, flags);
	}

	if ((!ret) || (!parent))
		goto out;

	if (sreg->mode == REGULATOR_MODE_FAST)
		return ret;

	while (ret) {
		wait_event(parent->wait_q ,
			   (max_uA - sreg->cur_uA <
			    parent->max_uA -
			    parent->cur_uA));
		spin_lock_irqsave(&parent->lock, flags);
		ret = main_add_current(parent, max_uA - sreg->cur_uA);
		spin_unlock_irqrestore(&parent->lock, flags);
	}
out:
	if (parent && (max_uA - sreg->cur_uA < 0))
		wake_up_all(&parent->wait_q);
	sreg->cur_uA = max_uA;
	return 0;
}

static int mxs_get_current_limit(struct regulator_dev *reg)
{
	struct mxs_regulator *sreg = rdev_get_drvdata(reg);

	return sreg->cur_uA;
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

static struct regulator_ops mxs_cur_rops = {
	.set_current_limit	= mxs_set_current_limit,
	.get_current_limit	= mxs_get_current_limit,
	.set_mode	= mxs_set_mode,
	.get_mode	= mxs_get_mode,
};

#define MXS_VDDD	0
#define MXS_VDDA	1
#define MXS_VDDIO	2
#define MXS_OVERALL_CUR	3

static struct regulator_desc mxs_reg_desc[] = {
	{
		.name = "vddd",
		.supply_name = "vdda",
		.id = MXS_VDDD,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
	},
	{
		.name = "vdda",
		.supply_name = "vddio",
		.id = MXS_VDDA,
		.type = REGULATOR_VOLTAGE,
		.n_voltages = 0x1f,
		.uV_step = 25000,
		.linear_min_sel = 0,
		.vsel_mask = 0x1f,
	},
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
		.name = "overall_current",
		.id = MXS_OVERALL_CUR,
		.type = REGULATOR_CURRENT,
		.linear_min_sel = 0,
	},
};	

static int reg_callback(struct notifier_block *self,
			unsigned long event, void *data)
{
	unsigned long flags;
	struct mxs_regulator *sreg =
		container_of(self, struct mxs_regulator , nb);

	switch (event) {
	case MXS_REG5V_IS_USB:
		spin_lock_irqsave(&sreg->lock, flags);
		sreg->max_uA = 500000;
		spin_unlock_irqrestore(&sreg->lock, flags);
		break;
	case MXS_REG5V_NOT_USB:
		spin_lock_irqsave(&sreg->lock, flags);
		sreg->max_uA = 0x7fffffff;
		spin_unlock_irqrestore(&sreg->lock, flags);
		break;
	}

	return 0;
}

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

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	initdata = of_get_regulator_init_data(dev, np);
	if (!initdata)
		return -EINVAL;

	/* get device base address */
	base_addr = of_iomap(np, 0);
	if (!base_addr)
		return -ENXIO;

	parent = of_get_parent(np);
	if (!parent) {
		ret = -ENXIO;
		goto fail2;
	}

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

	dev_info(dev, "regulator found\n");

	sreg = devm_kzalloc(dev, sizeof(*sreg), GFP_KERNEL);
	if (!sreg) {
		ret = -ENOMEM;
		goto fail3;
	}
	sreg->initdata = initdata;
	sreg->name = kstrdup(of_get_property(np, "regulator-name", NULL),
			     GFP_KERNEL);
	if (!sreg->name) {
		ret = -ENOMEM;
		goto fail3;
	}
	sreg->cur_uA = 0;
	sreg->cur_uV = 0;
	sreg->base_addr = base_addr;
	sreg->power_addr = power_addr;
	init_waitqueue_head(&sreg->wait_q);
	spin_lock_init(&sreg->lock);

	rdesc = &sreg->rdesc;
	rdesc->name = sreg->name;
	rdesc->owner = THIS_MODULE;

	if (strcmp(rdesc->name, "overall_current") == 0) {
		rdesc->type = REGULATOR_CURRENT;
		rdesc->ops = &mxs_cur_rops;
	} else {
		rdesc->type = REGULATOR_VOLTAGE;
		rdesc->ops = &mxs_vol_rops;
	}

	con = &initdata->constraints;
	rdesc->min_uV = con->min_uV;
	rdesc->vsel_reg = regaddr64;

	config.dev = &pdev->dev;
	config.init_data = initdata;
	config.driver_data = sreg;
	config.of_node = np;

	dev_dbg(dev, "probing regulator %s %d\n", sreg->name, pdev->id);

	/* register regulator */
	rdev = devm_regulator_register(dev, rdesc, &config);

	if (IS_ERR(rdev)) {
		dev_err(dev, "failed to register %s\n", sreg->name);
		ret = PTR_ERR(rdev);
		goto fail4;
	}

	if (sreg->max_uA) {
		struct regulator *regu;

		regu = regulator_get(NULL, sreg->name);
		sreg->nb.notifier_call = reg_callback;
		regulator_register_notifier(regu, &sreg->nb);
	}

	platform_set_drvdata(pdev, rdev);

	return 0;
fail4:
	kfree(sreg->name);
fail3:
	iounmap(power_addr);
fail2:
	iounmap(base_addr);

	return ret;
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
