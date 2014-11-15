/*
 * Freescale MXS power subsystem
 *
 * Stefan Wahren <stefan.wahren@i2se.com>
 *
 * Copyright (C) 2014 Stefan Wahren
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/types.h>

#define BM_POWER_CTRL_POLARITY_VBUSVALID	(1 << 5)
#define BM_POWER_CTRL_VBUSVALID_IRQ		(1 << 4)
#define BM_POWER_CTRL_ENIRQ_VBUS_VALID		(1 << 3)

#define HW_POWER_5VCTRL_OFFSET	0x10
#define HW_POWER_MISC_OFFSET	0x90

#define BM_POWER_5VCTRL_VBUSVALID_THRESH	(7 << 8)
#define BM_POWER_5VCTRL_PWDN_5VBRNOUT		(1 << 7)
#define BM_POWER_5VCTRL_VBUSVALID_5VDETECT	(1 << 4)

#define HW_POWER_5VCTRL_VBUSVALID_THRESH_4_40V	(5 << 8)

#define SHIFT_FREQSEL	4

#define BM_POWER_MISC_FREQSEL			(7 << SHIFT_FREQSEL)

#define HW_POWER_MISC_FREQSEL_20000_KHZ		1
#define HW_POWER_MISC_FREQSEL_24000_KHZ		2
#define HW_POWER_MISC_FREQSEL_19200_KHZ		3

#define HW_POWER_MISC_SEL_PLLCLK		(1 << 0)

static int dcdc_pll;
module_param(dcdc_pll, int, 0);
MODULE_PARM_DESC(dcdc_pll,
		 "DC-DC PLL frequency (kHz). Use 19200, 20000 or 24000");

struct mxs_power_data {
	void __iomem *base_addr;
	int irq;
	spinlock_t lock;

	struct power_supply dc;
};

int get_dcdc_clk_freq(struct mxs_power_data *pdata)
{
	void __iomem *base = pdata->base_addr;
	int ret = -EINVAL;
	u32 val;

	val = readl(base + HW_POWER_MISC_OFFSET);

	/* XTAL */
	if ((val & HW_POWER_MISC_SEL_PLLCLK) == 0)
		return 24000;

	switch ((val & BM_POWER_MISC_FREQSEL) >> SHIFT_FREQSEL) {
	case HW_POWER_MISC_FREQSEL_20000_KHZ:
		ret = 20000;
		break;
	case HW_POWER_MISC_FREQSEL_24000_KHZ:
		ret = 24000;
		break;
	case HW_POWER_MISC_FREQSEL_19200_KHZ:
		ret = 19200;
		break;
	}

	return ret;
}

int set_dcdc_clk_freq(struct mxs_power_data *pdata, int kHz)
{
	void __iomem *misc = pdata->base_addr + HW_POWER_MISC_OFFSET;
	u32 val;
	int ret = 0;

	val = readl(misc);

	val &= ~BM_POWER_MISC_FREQSEL;
	val &= ~HW_POWER_MISC_SEL_PLLCLK;

	/* Accept only values recommend by Freescale */
	switch (kHz) {
	case 19200:
		val |= HW_POWER_MISC_FREQSEL_19200_KHZ << SHIFT_FREQSEL;
		break;
	case 20000:
		val |= HW_POWER_MISC_FREQSEL_20000_KHZ << SHIFT_FREQSEL;
		break;
	case 24000:
		val |= HW_POWER_MISC_FREQSEL_24000_KHZ << SHIFT_FREQSEL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	/* First program FREQSEL */
	writel(val, misc);

	/* then set PLL as clk for DC-DC converter */
	writel(val | HW_POWER_MISC_SEL_PLLCLK, misc);

	return 0;
}

static enum power_supply_property mxs_power_dc_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int mxs_power_dc_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct mxs_power_data *data = container_of(psy,
						   struct mxs_power_data, dc);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static irqreturn_t mxs_power_interrupt(int irq, void *dev_id)
{
	unsigned long irq_flags;
	struct mxs_power_data *data = dev_id;

	spin_lock_irqsave(&data->lock, irq_flags);

	spin_unlock_irqrestore(&data->lock, irq_flags);

	return IRQ_HANDLED;
}

static const struct of_device_id of_mxs_power_match[] = {
	{ .compatible = "fsl,imx23-power", },
	{ .compatible = "fsl,imx28-power", },
	{ /* end */ }
};
MODULE_DEVICE_TABLE(of, of_mxs_power_match);

static int mxs_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	struct mxs_power_data *data;
	int ret;
	int dcdc_clk_freq;

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	data->dc.properties = mxs_power_dc_props;
	data->dc.num_properties = ARRAY_SIZE(mxs_power_dc_props);
	data->dc.get_property = mxs_power_dc_get_property;
	data->dc.name = "dc";
	data->dc.type = POWER_SUPPLY_TYPE_MAINS;

	spin_lock_init(&data->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base_addr = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->base_addr))
		return PTR_ERR(data->base_addr);

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0)
		return data->irq;

	ret = devm_request_irq(dev, data->irq, mxs_power_interrupt,
			       IRQF_SHARED, pdev->name, data);
	if (ret)
		return ret;

	ret = power_supply_register(dev, &data->dc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);

	if (dcdc_pll)
		set_dcdc_clk_freq(data, dcdc_pll);

	dcdc_clk_freq = get_dcdc_clk_freq(data);

	dev_info(dev, "DCDC clock freq: %d kHz\n", dcdc_clk_freq);

	return of_platform_populate(np, NULL, NULL, dev);
}

static int mxs_power_remove(struct platform_device *pdev)
{
	struct mxs_power_data *data = platform_get_drvdata(pdev);

	of_platform_depopulate(&pdev->dev);

	power_supply_unregister(&data->dc);

	return 0;
}

static struct platform_driver mxs_power_driver = {
	.driver = {
		.name	= "mxs_power",
		.owner  = THIS_MODULE,
		.of_match_table = of_mxs_power_match,
	},
	.probe	= mxs_power_probe,
	.remove = mxs_power_remove,
};

module_platform_driver(mxs_power_driver);

MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_DESCRIPTION("Freescale MXS power subsystem");
MODULE_LICENSE("GPL v2");
