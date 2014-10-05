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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

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

	if (!np) {
		dev_err(dev, "missing device tree\n");
		return -EINVAL;
	}

	return of_platform_populate(np, NULL, NULL, dev);
}

static int mxs_power_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);

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
