/*
 * Freescale eSPI controller driver.
 *
 * Copyright (C) 2008-2010 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author:
 * Jerry Huang <Chang-Ming.Huang@freescale.com>
 * Chen Gong <g.chen@freescale.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_bitbang.h>
#include <linux/platform_device.h>
#include <linux/fsl_devices.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_spi.h>
#include <sysdev/fsl_soc.h>
#include <linux/io.h>

#include <linux/gpio.h>
#include <linux/of_gpio.h>

/* SPI Controller registers */
struct fsl_espi_reg {
	__be32 mode;
	__be32 event;
	__be32 mask;
	__be32 command;
	__be32 transmit;
	__be32 receive;
	__be32 res[2];
	__be32 csmode[4];
};

#define MAX_CS_NUM		4

/* SPI Controller mode register definitions */
#define CSMODE_CI_INACTIVEHIGH	(1 << 31)
#define CSMODE_CP_BEGIN_EDGECLK	(1 << 30)
#define CSMODE_REV		(1 << 29)
#define CSMODE_DIV16		(1 << 28)
#define CSMODE_PM(x)		((x) << 24)
#define CSMODE_POL_1		(1 << 20)
#define CSMODE_LEN(x)		((x) << 16)
#define CS_BEF(x)		((x) << 12)
#define CS_AFT(x)		((x) << 8)
#define CS_CG(x)		((x) << 3)
#define CSMODE_PM_MAX		(0xF)
#define CSMODE_PM_MIN		(0x2)

#define SPMODE_ENABLE		(1 << 31)
#define SPMODE_LOOP		(1 << 30)
#define SPIMODE_TXTHR(x)	((x) << 8)
#define SPIMODE_RXTHR(x)	((x) << 0)

/*
 * Default for SPI Mode:
 * 	SPI MODE 0 (inactive low, phase middle, MSB, 8-bit length, slow clk
 */
#define CSMODE_INIT_VAL (CSMODE_POL_1 | CS_BEF(0) | CS_AFT(0) | CS_CG(1))  | CSMODE_CI_INACTIVEHIGH

#define SPMODE_INIT_VAL (SPIMODE_TXTHR(4) | SPIMODE_RXTHR(3))
//#define SPMODE_INIT_VAL (SPMODE_TXTHR(4) | SPMODE_RXTHR(3))
//#define CSMODE_INIT_VAL (CSMODE_POL_1 | CSMODE_BEF(0) \
//		| CSMODE_AFT(0) | CSMODE_CG(1)) | CSMODE_CI_INACTIVEHIGH

/* SPIE register values */
#define SPIE_RXT	0x00002000	/* RX Over Threshold */
#define SPIE_NE		0x00000200	/* Not empty */
#define SPIE_NF		0x00000100	/* Not full */
#define SPIE_RXCNT(reg)     ((reg >> 24) & 0x3F)
#define SPIE_TXCNT(reg)     ((reg >> 16) & 0x3F)

/* SPIM register values */
#define SPIM_NE		0x00000200	/* Not empty */
#define SPIM_NF		0x00000100	/* Not full */

/* the spi->mode bits understood by this driver: */
#define MODEBITS	(SPI_CPOL | SPI_CPHA | SPI_CS_HIGH \
			| SPI_LSB_FIRST | SPI_LOOP)

/* SPI Controller driver's private data. */
struct fsl_espi {
	/* bitbang has to be first */
	struct spi_bitbang bitbang;
	struct fsl_espi_reg __iomem *regs;
	struct completion done;

	/* rx & tx bufs from the spi_transfer */
	const void *tx;
	void *rx;

	/* functions to deal with different sized buffers */
	void (*get_rx) (u32 rx_data, struct fsl_espi *);
	u32 (*get_tx) (struct fsl_espi *);

	int count;
	int irq;

	u32 spibrg;		/* SPIBRG input clock */
	u32 rx_shift;		/* RX data reg shift when in qe mode */
	u32 tx_shift;		/* TX data reg shift when in qe mode */
};

struct fsl_espi_cs {
	/* functions to deal with different sized buffers */
	void (*get_rx) (u32 rx_data, struct fsl_espi *);
	u32 (*get_tx) (struct fsl_espi *);
	u32 rx_shift;		/* RX data reg shift when in qe mode */
	u32 tx_shift;		/* TX data reg shift when in qe mode */
	u32 hw_mode;		/* Holds HW mode register settings */
};

static void fsl_espi_rx_buf(u32 data, struct fsl_espi *fsl_espi)
{
	u32 *rx = fsl_espi->rx;
	*rx++ = (u32)(data >> fsl_espi->rx_shift);
	fsl_espi->rx = rx;
}

static u32 fsl_espi_tx_buf(struct fsl_espi *fsl_espi)
{
	u32 data;
	const u32 *tx = fsl_espi->tx;
	if (!tx)
		return 0;
	data = *tx++ << fsl_espi->tx_shift;
	fsl_espi->tx = tx;
	return data;
}

static
int fsl_espi_setup_transfer(struct spi_device *spi, struct spi_transfer *t)
{
	struct fsl_espi *fsl_espi;
	unsigned long flags;
	u32 regval;
	u8 bits_per_word, pm, cs_sel = spi->chip_select;
	u32 hz;
	struct fsl_espi_cs *cs = spi->controller_state;

	fsl_espi = spi_master_get_devdata(spi->master);

	if (t) {
		bits_per_word = t->bits_per_word;
		hz = t->speed_hz;
	} else {
		bits_per_word = 0;
		hz = 0;
	}

	/* spi_transfer level calls that work per-word */
	if (!bits_per_word)
		bits_per_word = spi->bits_per_word;

	/* Make sure its a bit width we support [4..16] */
	if (bits_per_word < 4 || bits_per_word > 16)
		return -EINVAL;
	bits_per_word -= 1;

	if (!hz)
		hz = spi->max_speed_hz;

	cs->rx_shift = 0;
	cs->tx_shift = 0;
	cs->get_rx = fsl_espi_rx_buf;
	cs->get_tx = fsl_espi_tx_buf;

	fsl_espi->rx_shift = cs->rx_shift;
	fsl_espi->tx_shift = cs->tx_shift;
	fsl_espi->get_rx = cs->get_rx;
	fsl_espi->get_tx = cs->get_tx;

	/* mask out bits we are going to set */
	cs->hw_mode &= ~(CSMODE_LEN(0xF) | CSMODE_DIV16 | CSMODE_PM(0xF));

	//cs->hw_mode |= CSMODE_LEN(bits_per_word) | CSMODE_INIT_VAL;
	cs->hw_mode |= CSMODE_LEN(bits_per_word); 
	if ((fsl_espi->spibrg / hz) > 32) {
		cs->hw_mode |= CSMODE_DIV16;
		pm = fsl_espi->spibrg / (hz * 32);
		if (pm > CSMODE_PM_MAX) {
			pm = CSMODE_PM_MAX;
			dev_err(&spi->dev, "Requested speed is too "
				"low: %d Hz. Will use %d Hz instead.\n",
				hz, fsl_espi->spibrg / 32 * 16);
		}
	} else {
		pm = fsl_espi->spibrg / (hz * 2);
/*delete by zhangjj 2015-11-12 change spi max HZ*/
#if 0
		if (pm < CSMODE_PM_MIN)
			pm = CSMODE_PM_MIN;
#endif
/*delete end*/
	}
	cs->hw_mode |= CSMODE_PM(pm);

	/* Reset the hw mode */
	regval = in_be32(&fsl_espi->regs->mode);
	local_irq_save(flags);
	/* Turn off SPI unit prior changing mode */
	out_be32(&fsl_espi->regs->mode, regval & ~SPMODE_ENABLE);
	out_be32(&fsl_espi->regs->csmode[cs_sel], cs->hw_mode);
	out_be32(&fsl_espi->regs->mode, regval);
	local_irq_restore(flags);

//      regval = in_be32(&fsl_espi->regs->mode);
//      printk("mode=0x%x\n",fsl_espi->regs->mode);
 //     regval = in_be32(&fsl_espi->regs->csmode[0]);
//      printk("csmode0=0x%x\n",fsl_espi->regs->csmode[0]);
//      regval = in_be32(&fsl_espi->regs->csmode[1]);
//      printk("csmode1=0x%x\n",fsl_espi->regs->csmode[1]);
//      regval = in_be32(&fsl_espi->regs->csmode[2]);
//      printk("csmode2=0x%x\n\n",fsl_espi->regs->csmode[2]);


	return 0;
}

static int fsl_espi_bufs(struct spi_device *spi, struct spi_transfer *t)
{
	struct fsl_espi *fsl_espi;
	u32 word, len, bits_per_word;
//	u32 cmd= 0;
//	u32 chip_sel_mode = 0;

//	printk("now read and write\n");
	fsl_espi = spi_master_get_devdata(spi->master);

//	fsl_espi->tx_shift = 8;
//	fsl_espi->rx_shift = 8;
//	spi->max_speed_hz = 2000000;
	//spi->chip_select = 0 ;// 0 fpga 1 dpll
//	spi->mode |=  SPI_CPHA | SPI_CPOL | SPI_CS_HIGH;
//	fsl_espi_setup_transfer(spi, NULL);

	fsl_espi->tx = t->tx_buf;
	fsl_espi->rx = t->rx_buf;
	bits_per_word = spi->bits_per_word;
	if (t->bits_per_word)
		bits_per_word = t->bits_per_word;
	len = t->len;
	fsl_espi->count = len;

	//cmd = in_be32(&fsl_espi->regs->command);
	//chip_sel_mode = in_be32(&fsl_espi->regs->csmode[spi->chip_select]) ;
	//printk(" cmd = 0x%08x , chip_sel_mode= 0x%08x   spi->chip_select  = 0x%08x\n", cmd, chip_sel_mode, spi->chip_select);
	/* every frame owns one byte */
	out_be32(&fsl_espi->regs->command,
			(spi->chip_select << 30) | (len - 1));
	INIT_COMPLETION(fsl_espi->done);

	/* enable rx ints */
	out_be32(&fsl_espi->regs->mask, SPIM_NE);

	/* transmit word */
	word = fsl_espi->get_tx(fsl_espi);
	out_be32(&fsl_espi->regs->transmit, word);
	
	//printk(" word  = 0x%08x , conut = 0x%08x,  fsl_espi->rx_shift = 0x%08x,,        fsl_espi->tx_shift= 0x%08x,,\n", word,fsl_espi->count , fsl_espi->rx_shift,   fsl_espi->tx_shift);

	wait_for_completion(&fsl_espi->done);
	/* disable rx ints */
	out_be32(&fsl_espi->regs->mask, 0);

#if 0
	{
			int i;
			for(i = 0; i < t->len; i++)
			{
				printk(" word  = 0x%08x       ", *(u8*)(t->rx_buf ++) );
			}
	}
	
#endif
	return t->len;
}

static void fsl_espi_chipselect(struct spi_device *spi, int is_on)
{
	return;
}

static int fsl_espi_setup(struct spi_device *spi)
{
	struct fsl_espi *fsl_espi;
	int retval;
	u32 hw_mode;
	struct fsl_espi_cs	*cs = spi->controller_state;

	if (spi->mode & ~MODEBITS) {
		dev_dbg(&spi->dev, "setup: unsupported mode bits %x\n",
			spi->mode & ~MODEBITS);
		return -EINVAL;
	}

	if (!spi->max_speed_hz)
		return -EINVAL;

	if (!cs) {
		cs = kzalloc(sizeof(*cs), GFP_KERNEL);
		if (!cs)
			return -ENOMEM;
		spi->controller_state = cs;
	}

	fsl_espi = spi_master_get_devdata(spi->master);

	if (!spi->bits_per_word)
		spi->bits_per_word = 8;

	hw_mode = cs->hw_mode; /* Save orginal settings */
	cs->hw_mode =
		in_be32(&fsl_espi->regs->csmode[spi->chip_select]);
	/* mask out bits we are going to set */
	cs->hw_mode &= ~(CSMODE_CP_BEGIN_EDGECLK | CSMODE_CI_INACTIVEHIGH
			 | CSMODE_REV);

	if (spi->mode & SPI_CPHA)
		cs->hw_mode |= CSMODE_CP_BEGIN_EDGECLK;
	if (spi->mode & SPI_CPOL)
		cs->hw_mode |= CSMODE_CI_INACTIVEHIGH;
	if (!(spi->mode & SPI_LSB_FIRST))
		cs->hw_mode |= CSMODE_REV;

	retval = fsl_espi_setup_transfer(spi, NULL);
	if (retval < 0) {
		cs->hw_mode = hw_mode; /* Restore settings */
		return retval;
	}

	dev_dbg(&spi->dev, "%s, mode %d, %u bits/w, %u Hz\n",
		__func__, spi->mode & (SPI_CPOL | SPI_CPHA),
		spi->bits_per_word, spi->max_speed_hz);
	return 0;
}

irqreturn_t fsl_espi_irq(s32 irq, void *context_data)
{
	struct fsl_espi *fsl_espi = context_data;
	u32 event, rx_data, word;
	int ret;

	/* Get interrupt events(tx/rx) */
	event = in_be32(&fsl_espi->regs->event);

	/* We need handle RX first */
	if (event & SPIE_NE) {
		/* spin until RX is done */
		void *event_ptr = &fsl_espi->regs->event;
		int limit = min(4, fsl_espi->count);

		ret = spin_event_timeout(
			SPIE_RXCNT(event = in_be32(event_ptr)) >=  limit,
			500, 0);
		if (!ret)
			return IRQ_NONE;
		rx_data = in_be32(&fsl_espi->regs->receive);

		if (fsl_espi->rx)
			fsl_espi->get_rx(rx_data, fsl_espi);
	} else {
		/* Clear the events */
		out_be32(&fsl_espi->regs->event, event);
		return IRQ_HANDLED;
	}

	fsl_espi->count -= 4;
	if (fsl_espi->count > 0) {
		if ((event & SPIE_NF) == 0) {
			/* spin until TX is done */
			ret = spin_event_timeout((event =
				in_be32(&fsl_espi->regs->event)) & SPIE_NF,
				500, 0);
			if (!ret)
				return IRQ_NONE;
		}
		word = fsl_espi->get_tx(fsl_espi);
		out_be32(&fsl_espi->regs->transmit, word);
	} else {
		fsl_espi->count = 0;
#if 0
		/* disable rx ints */
		out_be32(&fsl_espi->regs->mask, 0);
#else
                /* Clear the events */
                out_be32(&fsl_espi->regs->event, event);
#endif
		complete(&fsl_espi->done);
	}
#if 0
	/* Clear the events */
	out_be32(&fsl_espi->regs->event, event);
#endif
	return IRQ_HANDLED;
}

static void fsl_espi_cleanup(struct spi_device *spi)
{
	kfree(spi->controller_state);
	spi->controller_state = NULL;
}

#define 		GPIO_DIR				0x00
#define 		GPIO_ODR			0x04
#define 		GPIO_DAT			0x08
#define 		GPIO_IER				0x0c
#define 		GPIO_IMR			0x10
#define 		GPIO_ICR				0x14
#define 		MPC8XXX_GPIO_PINS		32

static inline u32 mpc8xxx_gpio2mask(unsigned int gpio)
{
	return 1u << (MPC8XXX_GPIO_PINS - 1 - gpio);
}


void reset_system(void)
{
	void __iomem *immap;

	immap = ioremap(get_immrbase()+0xf000, 0x10);
	clrbits32(immap+8, 0x10000000);
	clrbits32( immap, 0x40000000);
       iounmap(immap);

	return;
}


static int __init fsl_espi_probe(struct of_device *ofdev,
					const struct of_device_id *match)
{
	struct spi_master *master;
	struct fsl_espi *fsl_espi;
	struct resource r_irq_struct;
	struct resource r_mem_struct;
	struct resource *r_irq = &r_irq_struct;
	struct resource *r_mem = &r_mem_struct;
	const u32 *prop;
	u32 regval;
	int ret = 0;
	int len;

			printk(" now probe begin *******************\n");
	{
	#if 1
		int  gpio =   6;
		int  fpga_config =8;
		int i = 0;
		struct device *dev = &ofdev->dev;
		
		if (!gpio_is_valid(gpio)) {
			dev_err(dev, "invalid gpio #%d: %d\n", i, gpio);
			ret = gpio;
		}
		/*add for fpgaupdate by zhangjj 2015-12-31*/
		ret = gpio_request(12, dev_name(dev));
		if (ret) {
			dev_err(dev, "can't request gpio #%d: %d\n", i, ret);
		}
		/*add end*/
		ret = gpio_request(gpio, dev_name(dev));
		if (ret) {
			dev_err(dev, "can't request gpio #%d: %d\n", i, ret);
		}

		

		ret = gpio_direction_output(gpio,  1);;
		if (ret) {
			dev_err(dev, "can't set output direction for gpio "
				"#%d: %d\n", i, ret);
		}

		{
				
void __iomem *immap;
int d;

				immap = ioremap(get_immrbase()+0xe0060,8);
	
        				  d = mpc8xxx_gpio2mask(1);	
	 			 clrbits32( immap, d);
          			iounmap(immap);
		}
		if (!gpio_is_valid(fpga_config)) {
			dev_err(dev, "invalid gpio #%d: %d\n", i, fpga_config);
			ret = fpga_config;
		}

		ret = gpio_request(fpga_config, dev_name(dev));
		if (ret) {
			dev_err(dev, "can't request gpio #%d: %d\n", i, ret);
		}

		

		ret = gpio_direction_output(fpga_config, 0); //sxl
		if (ret) {
			dev_err(dev, "can't set output direction for gpio "
				"#%d: %d\n", i, ret);
		}
	#endif
	}
	
	/* Get resources(memory, IRQ) associated with the device */
	master = spi_alloc_master(&ofdev->dev, sizeof(struct fsl_espi));

	if (master == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	dev_set_drvdata(&ofdev->dev, master);

	ret = of_address_to_resource(ofdev->dev.of_node, 0, r_mem);
	if (ret) {
		dev_warn(&ofdev->dev, "invalid address\n");
		goto free_master;
	}

	ret = of_irq_to_resource(ofdev->dev.of_node, 0, r_irq);
	if (ret == NO_IRQ) {
		dev_warn(&ofdev->dev, "no IRQ found\n");
		goto free_master;
	}

	//add sxl
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;

	master->setup = fsl_espi_setup;
	master->cleanup = fsl_espi_cleanup;

	fsl_espi = spi_master_get_devdata(master);
	fsl_espi->bitbang.master = spi_master_get(master);
	fsl_espi->bitbang.chipselect = fsl_espi_chipselect;
	fsl_espi->bitbang.txrx_bufs = fsl_espi_bufs;
	fsl_espi->bitbang.setup_transfer = fsl_espi_setup_transfer;
	fsl_espi->get_rx = fsl_espi_rx_buf;
	fsl_espi->get_tx = fsl_espi_tx_buf;
	fsl_espi->spibrg = fsl_get_sys_freq() / 2;

	fsl_espi->rx_shift = 0;
	fsl_espi->tx_shift = 0;

	init_completion(&fsl_espi->done);

	if (!request_mem_region(r_mem->start,
				r_mem->end - r_mem->start + 1, "fsl-espi")) {
		ret = -ENXIO;
		dev_warn(&ofdev->dev, "memory request failure\n");
		goto free_master;
	}

	fsl_espi->regs = ioremap(r_mem->start, r_mem->end - r_mem->start + 1);
	if (fsl_espi->regs == NULL) {
		ret = -ENOMEM;
		goto put_master;
	}

	fsl_espi->irq = r_irq->start;
	if (fsl_espi->irq < 0) {
		ret = -ENXIO;
		goto unmap_io;
	}

	/* Register for SPI Interrupt */
	ret = request_irq(fsl_espi->irq, fsl_espi_irq,
			  0, "fsl-espi", fsl_espi);

	if (ret != 0)
		goto unmap_io;

	master->bus_num = -1;	/* dynamic bus assignment */
	/* number of slave select bits is required */
	prop = of_get_property(ofdev->dev.of_node, "espi,num-ss-bits", &len);
	if (!prop || len < sizeof(*prop)) {
		dev_warn(&ofdev->dev, "no 'espi,num-ss-bits' property\n");
		goto free_irq;
	}
	master->num_chipselect = *prop;

	/* SPI controller initializations */
	out_be32(&fsl_espi->regs->mode, 0);
	out_be32(&fsl_espi->regs->mask, 0);
	out_be32(&fsl_espi->regs->command, 0);
	out_be32(&fsl_espi->regs->event, 0xffffffff);

	/* Enable SPI interface */
	regval = SPMODE_INIT_VAL | SPMODE_ENABLE;
	out_be32(&fsl_espi->regs->mode, regval);
	/* init CS mode interface */
	for (ret = 0; ret < MAX_CS_NUM; ret++)
		out_be32(&fsl_espi->regs->csmode[ret],
				CSMODE_INIT_VAL);

	ret = spi_bitbang_start(&fsl_espi->bitbang);
	if (ret != 0) {
		dev_err(&ofdev->dev, "spi_bitbang_start FAILED\n");
		goto free_irq;
	}

	dev_info(&ofdev->dev,
	       "Freescale eSPI Controller driver at 0x%p (irq = %d)\n",
	       fsl_espi->regs, fsl_espi->irq);

	/* add any subnodes on the SPI bus */
	of_register_spi_devices(master, ofdev->dev.of_node);

	return ret;

free_irq:
	free_irq(fsl_espi->irq, fsl_espi);
unmap_io:
	iounmap(fsl_espi->regs);
put_master:
	spi_master_put(master);
free_master:
	kfree(master);
err:
	return ret;
}

static int __exit fsl_espi_remove(struct of_device *ofdev)
{
	struct fsl_espi *fsl_espi;
	struct spi_master *master;
	struct resource r_mem;

	master = platform_get_drvdata(ofdev);
	fsl_espi = spi_master_get_devdata(master);

	spi_bitbang_stop(&fsl_espi->bitbang);

	free_irq(fsl_espi->irq, fsl_espi);
	iounmap(fsl_espi->regs);
	fsl_espi->regs = NULL;
	if (!of_address_to_resource(ofdev->dev.of_node, 0, &r_mem))
		release_mem_region(r_mem.start, resource_size(&r_mem));
	dev_set_drvdata(&ofdev->dev, 0);
	spi_master_put(fsl_espi->bitbang.master);

	return 0;
}

static struct of_device_id fsl_espi_of_match[] = {
	{ .compatible = "fsl,espi",},
	{}
};
MODULE_DEVICE_TABLE(of, fsl_espi_of_match);

MODULE_ALIAS("platform:fsl-espi");
static struct of_platform_driver fsl_espi_driver = {
	.driver = {
		.name = "fsl-espi",
		.owner = THIS_MODULE,
		.of_match_table = fsl_espi_of_match,
	},
	.probe = fsl_espi_probe,
	.remove = __exit_p(fsl_espi_remove),
};

static int __init fsl_espi_init(void)
{
	return of_register_platform_driver(&fsl_espi_driver);
}

static void __exit fsl_espi_exit(void)
{
	of_unregister_platform_driver(&fsl_espi_driver);
}
#if 0 /*delete kernel mode spi by zhangjj 2016-10-18*/
module_init(fsl_espi_init);
module_exit(fsl_espi_exit);

MODULE_AUTHOR("Chen Gong <g.chen@freescale.com>, "
	      "Jerry Huang <Chang-Ming.Huang@freescale.com>");
MODULE_DESCRIPTION("Freescale eSPI Driver");
MODULE_LICENSE("GPL");
#endif
