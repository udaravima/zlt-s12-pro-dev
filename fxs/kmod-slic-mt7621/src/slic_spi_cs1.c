// SPDX-License-Identifier: GPL-2.0
/*
 * slic_spi_cs1 - register the Le9641 SLIC on MT7621 SPI0 chip-select 1.
 *
 * The OpenWrt DT for the ZLT S12 Pro only declares the NOR flash on CS0.
 * Stock Tozed firmware had `spidev@1 { compatible = "rohm,dh2228fv"; }`.
 * This module recreates that device at runtime so the in-tree spidev driver
 * binds and exposes /dev/spidev0.1, without rebuilding the DTB.
 *
 * Note: spi-mt7621 forces SPI mode 0 on every message (other modes garble
 * bits on this SoC), so the stock mode 3 cannot be used.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

static unsigned int max_hz = 1000000;
module_param(max_hz, uint, 0444);
MODULE_PARM_DESC(max_hz, "SPI clock limit in Hz (stock used 2048000)");

static struct spi_device *slic_spi;

static int match_spi_controller(struct device *dev, void *data)
{
	return dev->class && !strcmp(dev->class->name, "spi_master");
}

static int __init slic_spi_cs1_init(void)
{
	struct spi_board_info info = {
		.modalias = "dh2228fv",		/* in spidev's id table */
		.max_speed_hz = max_hz,
		.chip_select = 1,
		.mode = SPI_MODE_0,
	};
	struct platform_device *pdev;
	struct spi_controller *ctlr;
	struct device_node *np;
	struct device *dev;

	np = of_find_compatible_node(NULL, NULL, "ralink,mt7621-spi");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;

	dev = device_find_child(&pdev->dev, NULL, match_spi_controller);
	platform_device_put(pdev);
	if (!dev)
		return -ENODEV;
	ctlr = container_of(dev, struct spi_controller, dev);

	slic_spi = spi_new_device(ctlr, &info);
	put_device(dev);
	if (!slic_spi)
		return -EIO;

	dev_info(&slic_spi->dev, "SLIC registered on CS1, %u Hz, mode 0\n", max_hz);
	return 0;
}

static void __exit slic_spi_cs1_exit(void)
{
	spi_unregister_device(slic_spi);
}

module_init(slic_spi_cs1_init);
module_exit(slic_spi_cs1_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Register Le9641 SLIC spidev on MT7621 SPI0 CS1");
