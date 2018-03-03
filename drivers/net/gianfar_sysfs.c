/*
 * drivers/net/gianfar_sysfs.c
 *
 * Gianfar Ethernet Driver
 * This driver is designed for the non-CPM ethernet controllers
 * on the 85xx and 83xx family of integrated processors
 * Based on 8260_io/fcc_enet.c
 *
 * Author: Andy Fleming
 * Maintainer: Kumar Gala (galak@kernel.crashing.org)
 * Modifier: Sandeep Gopalpet <sandeep.kumar@freescale.com>
 *
 * Copyright 2002-2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * Sysfs file creation and management
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/device.h>

#include <asm/uaccess.h>
#include <linux/module.h>

#include "gianfar.h"

static ssize_t gfar_show_bd_stash(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%s\n", priv->bd_stash_en ? "on" : "off");
}

static ssize_t gfar_set_bd_stash(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	int new_setting = 0;
	u32 temp;
	unsigned long flags;

	if (!(priv->device_flags & FSL_GIANFAR_DEV_HAS_BD_STASHING))
		return count;


	/* Find out the new setting */
	if (!strncmp("on", buf, count - 1) || !strncmp("1", buf, count - 1))
		new_setting = 1;
	else if (!strncmp("off", buf, count - 1) ||
		 !strncmp("0", buf, count - 1))
		new_setting = 0;
	else
		return count;


	local_irq_save(flags);
	lock_rx_qs(priv);

	/* Set the new stashing value */
	priv->bd_stash_en = new_setting;

	temp = gfar_read(&regs->attr);

	if (new_setting)
		temp |= ATTR_BDSTASH;
	else
		temp &= ~(ATTR_BDSTASH);

	gfar_write(&regs->attr, temp);

	unlock_rx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(bd_stash, 0644, gfar_show_bd_stash, gfar_set_bd_stash);

static ssize_t gfar_show_rx_stash_size(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->rx_stash_size);
}

static ssize_t gfar_set_rx_stash_size(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned int length = simple_strtoul(buf, NULL, 0);
	u32 temp;
	unsigned long flags;

	if (!(priv->device_flags & FSL_GIANFAR_DEV_HAS_BUF_STASHING))
		return count;

	local_irq_save(flags);
	lock_rx_qs(priv);

	if (length > priv->rx_buffer_size)
		goto out;

	if (length == priv->rx_stash_size)
		goto out;

	priv->rx_stash_size = length;

	temp = gfar_read(&regs->attreli);
	temp &= ~ATTRELI_EL_MASK;
	temp |= ATTRELI_EL(length);
	gfar_write(&regs->attreli, temp);

	/* Turn stashing on/off as appropriate */
	temp = gfar_read(&regs->attr);

	if (length)
		temp |= ATTR_BUFSTASH;
	else
		temp &= ~(ATTR_BUFSTASH);

	gfar_write(&regs->attr, temp);

out:
	unlock_rx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(rx_stash_size, 0644, gfar_show_rx_stash_size,
		   gfar_set_rx_stash_size);

/* Stashing will only be enabled when rx_stash_size != 0 */
static ssize_t gfar_show_rx_stash_index(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->rx_stash_index);
}

static ssize_t gfar_set_rx_stash_index(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned short index = simple_strtoul(buf, NULL, 0);
	u32 temp;
	unsigned long flags;

	if (!(priv->device_flags & FSL_GIANFAR_DEV_HAS_BUF_STASHING))
		return count;

	local_irq_save(flags);
	lock_rx_qs(priv);

	if (index > priv->rx_stash_size)
		goto out;

	if (index == priv->rx_stash_index)
		goto out;

	priv->rx_stash_index = index;

	temp = gfar_read(&regs->attreli);
	temp &= ~ATTRELI_EI_MASK;
	temp |= ATTRELI_EI(index);
	gfar_write(&regs->attreli, temp);

out:
	unlock_rx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(rx_stash_index, 0644, gfar_show_rx_stash_index,
		   gfar_set_rx_stash_index);

static ssize_t gfar_show_fifo_threshold(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->fifo_threshold);
}

static ssize_t gfar_set_fifo_threshold(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned int length = simple_strtoul(buf, NULL, 0);
	u32 temp;
	unsigned long flags;

	if (length > GFAR_MAX_FIFO_THRESHOLD)
		return count;

	local_irq_save(flags);
	lock_tx_qs(priv);

	priv->fifo_threshold = length;

	temp = gfar_read(&regs->fifo_tx_thr);
	temp &= ~FIFO_TX_THR_MASK;
	temp |= length;
	gfar_write(&regs->fifo_tx_thr, temp);

	unlock_tx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(fifo_threshold, 0644, gfar_show_fifo_threshold,
		   gfar_set_fifo_threshold);

static ssize_t gfar_show_fifo_starve(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->fifo_starve);
}

static ssize_t gfar_set_fifo_starve(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned int num = simple_strtoul(buf, NULL, 0);
	u32 temp;
	unsigned long flags;

	if (num > GFAR_MAX_FIFO_STARVE)
		return count;

	local_irq_save(flags);
	lock_tx_qs(priv);

	priv->fifo_starve = num;

	temp = gfar_read(&regs->fifo_tx_starve);
	temp &= ~FIFO_TX_STARVE_MASK;
	temp |= num;
	gfar_write(&regs->fifo_tx_starve, temp);

	unlock_tx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(fifo_starve, 0644, gfar_show_fifo_starve,
		   gfar_set_fifo_starve);

static ssize_t gfar_show_fifo_starve_off(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->fifo_starve_off);
}

static ssize_t gfar_set_fifo_starve_off(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned int num = simple_strtoul(buf, NULL, 0);
	u32 temp;
	unsigned long flags;

	if (num > GFAR_MAX_FIFO_STARVE_OFF)
		return count;

	local_irq_save(flags);
	lock_tx_qs(priv);

	priv->fifo_starve_off = num;

	temp = gfar_read(&regs->fifo_tx_starve_shutoff);
	temp &= ~FIFO_TX_STARVE_OFF_MASK;
	temp |= num;
	gfar_write(&regs->fifo_tx_starve_shutoff, temp);

	unlock_tx_qs(priv);
	local_irq_restore(flags);

	return count;
}

static DEVICE_ATTR(fifo_starve_off, 0644, gfar_show_fifo_starve_off,
		   gfar_set_fifo_starve_off);

#ifdef CONFIG_GFAR_SKBUFF_RECYCLING
static ssize_t gfar_show_recycle_max(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	return sprintf(buf, "%d\n",
		priv->rx_queue[0]->skb_handler.recycle_max);
}

static ssize_t gfar_set_recycle_max(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));
	unsigned int length = simple_strtoul(buf, NULL, 0);
	int i = 0;

	/* recycling max management is loosely done. If the count is more
	 * than max, simply don't keep the buffer until the current amount
	 * lower than max.
	 */
	for (i = 0; i < priv->num_rx_queues; i++)
		priv->rx_queue[i]->skb_handler.recycle_max = length;
	return count;
}

static DEVICE_ATTR(recycle_max, 0644, gfar_show_recycle_max,
				gfar_set_recycle_max);
#endif

static ssize_t gfar_show_max_filer_rules(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct gfar_private *priv = netdev_priv(to_net_dev(dev));

	return sprintf(buf, "%d\n", priv->max_filer_rules);
}

static DEVICE_ATTR(max_filer_rules, 0444, gfar_show_max_filer_rules,
				NULL);

void gfar_init_sysfs(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	int rc;

	/* Initialize the default values */
	priv->fifo_threshold = DEFAULT_FIFO_TX_THR;
	priv->fifo_starve = DEFAULT_FIFO_TX_STARVE;
	priv->fifo_starve_off = DEFAULT_FIFO_TX_STARVE_OFF;

	/* Create our sysfs files */
	rc = device_create_file(&dev->dev, &dev_attr_bd_stash);
	rc |= device_create_file(&dev->dev, &dev_attr_rx_stash_size);
	rc |= device_create_file(&dev->dev, &dev_attr_rx_stash_index);
	rc |= device_create_file(&dev->dev, &dev_attr_fifo_threshold);
	rc |= device_create_file(&dev->dev, &dev_attr_fifo_starve);
	rc |= device_create_file(&dev->dev, &dev_attr_fifo_starve_off);
#ifdef CONFIG_GFAR_SKBUFF_RECYCLING
	rc |= device_create_file(&dev->dev, &dev_attr_recycle_max);
#endif
	rc |= device_create_file(&dev->dev, &dev_attr_max_filer_rules);
	if (rc)
		dev_err(&dev->dev, "Error creating gianfar sysfs files.\n");
}
