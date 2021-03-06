/*
 * P1022 DS Board Setup
 *
 * Copyright 2010 - 2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/memblock.h>

#include <asm/system.h>
#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <mm/mmu_decl.h>
#include <asm/prom.h>
#include <asm/udbg.h>
#include <asm/mpic.h>
#include <asm/swiotlb.h>
#include <asm/immap_85xx.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>

#if defined(CONFIG_FB_FSL_DIU) || defined(CONFIG_FB_FSL_DIU_MODULE)
/*Added by suipl 2013.2.25 for JH1022 *********/
static u32 get_busfreq(void)
{
	struct device_node *node;

	u32 fs_busfreq = 0;
	node = of_find_node_by_type(NULL, "cpu");
	if (node) {
		unsigned int size;
		const unsigned int *prop =
			of_get_property(node, "bus-frequency", &size);
		if (prop)
			fs_busfreq = *prop;
		of_node_put(node);
	};
	return fs_busfreq;
}
/*********************************************/

unsigned int p1022ds_get_pixel_format(unsigned int bits_per_pixel,
					int monitor_port)
{
	static const unsigned long pixelformat[][3] = {
		{0x88883316, 0x88082219, 0x65053118},
		{0x88883316, 0x88082219, 0x65053118},
	};
	unsigned int pix_fmt, arch_monitor;

	/* Fixed ME: monitor_port is 1? */
	arch_monitor = (monitor_port == 0) ? 0 : 1;

	if (bits_per_pixel == 32)
		pix_fmt = pixelformat[monitor_port][0];
	else if (bits_per_pixel == 24)
		pix_fmt = pixelformat[monitor_port][1];
	else if (bits_per_pixel == 16)
		pix_fmt = pixelformat[monitor_port][2];
	else
		pix_fmt = pixelformat[2][0];

	return pix_fmt;
}

void p1022ds_set_gamma_table(int monitor_port, char *gamma_table_base)
{
	int i;

	if (monitor_port == 2) {		/* dual link LVDS */
		for (i = 0; i < 256*3; i++)
			gamma_table_base[i] = (gamma_table_base[i] << 2) |
					 ((gamma_table_base[i] >> 6) & 0x03);
	}
}

void p1022ds_set_monitor_port(int monitor_port)
{
	return;
}

void p1022ds_set_pixel_clock(unsigned int pixclock)
{
#if 0
	struct device_node *guts_np = NULL;
	struct ccsr_guts __iomem *guts;
	unsigned long freq;
	u64 temp;
	u32 pxclk;

	/* Map the global utilities registers. */
	guts_np = of_find_compatible_node(NULL, NULL, "fsl,p1022-guts");
	if (!guts_np) {
		printk(KERN_ERR "Could not find GUTS node\n");
		return;
	}

	guts = of_iomap(guts_np, 0);
	of_node_put(guts_np);
	if (!guts) {
		printk(KERN_ERR "Failed to map GUTS register space\n");
		return;
	}

	/* Convert pixclock from a wavelength to a frequency */
	temp = 1000000000000ULL;
	do_div(temp, pixclock);
	freq = temp;

	/* pixclk is the ratio of the platform clock to the pixel clock */
	pxclk = DIV_ROUND_CLOSEST(fsl_get_sys_freq(), freq);

	/* Disable the pixel clock, and set it to non-inverted and no delay */
	clrbits32(&guts->clkdvdr, CCSR_GUTS_CLKDVDR_PXCKEN |
		CCSR_GUTS_CLKDVDR_PXCKDLY | CCSR_GUTS_CLKDVDR_PXCLK_MASK);

	/* Enable the clock and set the pxclk */
	setbits32(&guts->clkdvdr, CCSR_GUTS_CLKDVDR_PXCKEN | (pxclk << 16));

	iounmap(guts);
#endif
/****Changed by suipl 2013.2.25 for JH1022 *********/
	u32 __iomem *clkdvdr;
	u32 temp;
	/* variables for pixel clock calcs */
	ulong  bestval, bestfreq, speed_ccb, minpixclock, maxpixclock;
	ulong pixval;
	long err;
	int i;

	clkdvdr = ioremap(get_immrbase() + 0xe0800, sizeof(u32));
	if (!clkdvdr) {
		printk(KERN_ERR "Err: can't map clock divider register!\n");
		return;
	}

	/* Pixel Clock configuration */
	pr_debug("DIU: Bus Frequency = %d\n", get_busfreq());
	speed_ccb = get_busfreq();

	/* Calculate the pixel clock with the smallest error */
	/* calculate the following in steps to avoid overflow */
	pr_debug("DIU pixclock in ps - %d\n", pixclock);
	temp = 1000000000/pixclock;
	temp *= 1000;
	pixclock = temp;
	pr_debug("DIU pixclock freq - %u\n", pixclock);

	temp = pixclock * 5 / 100;
	pr_debug("deviation = %d\n", temp);
	minpixclock = pixclock - temp;
	maxpixclock = pixclock + temp;
	pr_debug("DIU minpixclock - %lu\n", minpixclock);
	pr_debug("DIU maxpixclock - %lu\n", maxpixclock);
	pixval = speed_ccb/pixclock;
	pr_debug("DIU pixval = %lu\n", pixval);

	err = 100000000;
	bestval = pixval;
	pr_debug("DIU bestval = %lu\n", bestval);

	bestfreq = 0;
	for (i = -1; i <= 1; i++) {
		temp = speed_ccb / ((pixval+i) + 1);
		pr_debug("DIU test pixval i= %d, pixval=%lu, temp freq. = %u\n",
							i, pixval, temp);
		if ((temp < minpixclock) || (temp > maxpixclock))
			pr_debug("DIU exceeds monitor range (%lu to %lu)\n",
				minpixclock, maxpixclock);
		else if (abs(temp - pixclock) < err) {
			pr_debug("Entered the else if block %d\n", i);
			err = abs(temp - pixclock);
			bestval = pixval+i;
			bestfreq = temp;
		}
	}

	pr_debug("DIU chose = %lx\n", bestval);
	pr_debug("DIU error = %ld\n NomPixClk ", err);
	pr_debug("DIU: Best Freq = %lx\n", bestfreq);
	/* Modify PXCLK in GUTS CLKDVDR */
	pr_debug("DIU: Current value of CLKDVDR = 0x%08x\n", (*clkdvdr));
	temp = (*clkdvdr) & 0x2000FFFF;
	*clkdvdr = temp;		/* turn off clock */
	*clkdvdr = temp | 0x80000000 | (((bestval) & 0x1F) << 16);
	pr_debug("DIU: Modified value of CLKDVDR = 0x%08x\n", (*clkdvdr));
	iounmap(clkdvdr);

}

ssize_t p1022ds_show_monitor_port(int monitor_port, char *buf)
{
	return snprintf(buf, PAGE_SIZE,
			"%c0 - DVI\n"
			"%c1 - Single link LVDS\n",
			monitor_port == 0 ? '*' : ' ',
			monitor_port == 1 ? '*' : ' ');
}

int p1022ds_set_sysfs_monitor_port(int val)
{
	return val < 3 ? val : 0;
}
#endif /* defined(CONFIG_FB_FSL_DIU) || defined(CONFIG_FB_FSL_DIU_MODULE) */

void __init p1022_ds_pic_init(void)
{
	struct mpic *mpic;
	struct resource r;
	struct device_node *np;
	unsigned long root = of_get_flat_dt_root();

	np = of_find_node_by_type(NULL, "open-pic");
	if (np == NULL) {
		printk(KERN_ERR "Could not find open-pic node\n");
		return;
	}

	if (of_address_to_resource(np, 0, &r)) {
		printk(KERN_ERR "Failed to map mpic register space\n");
		of_node_put(np);
		return;
	}

	if (of_flat_dt_is_compatible(root, "fsl,P1022DS-CAMP")) {
		mpic = mpic_alloc(np, r.start,
			MPIC_PRIMARY |
			MPIC_BIG_ENDIAN | MPIC_BROKEN_FRR_NIRQS,
			0, 256, " OpenPIC  ");
	} else {
		mpic = mpic_alloc(np, r.start,
			MPIC_PRIMARY | MPIC_WANTS_RESET |
			MPIC_BIG_ENDIAN | MPIC_BROKEN_FRR_NIRQS |
			MPIC_SINGLE_DEST_CPU,
			0, 256, " OpenPIC  ");
	}

	BUG_ON(mpic == NULL);
	of_node_put(np);

	mpic_init(mpic);
}

/*Host/agent status can be read from porbmsr in the global utilities*/
static int get_p1022ds_host_agent(void)
{
	struct device_node *np;
	void __iomem *gur_regs;
	u32 host_agent;

	np = of_find_compatible_node(NULL, NULL, "fsl,p1022-guts");
	if (np == NULL) {
		printk(KERN_ERR "Could not find global-utilities node\n");
		return 0;
	}

	gur_regs = of_iomap(np, 0);
	of_node_put(np);

	if (!gur_regs) {
		printk(KERN_ERR "Failed to map global-utilities register space\n");
		return 0;
	}
	host_agent = (in_be32(gur_regs + 4) & 0x00070000) >> 16 ;

	iounmap(gur_regs);

	return host_agent;
}

/*
 * To judge if the PCI(e) controller is host/agent mode through
 * the PORBMSR register.
 * 0: agent mode
 * 1: host mode
 */
static int p1022ds_pci_is_host(u32 host_agent, resource_size_t res)
{
	switch (res & 0xfffff) {
	case 0xa000:	/* PCIe1 */
		if (host_agent == 0 || host_agent == 1 ||
			host_agent == 2 || host_agent == 3)
			return 0;
		break;
	case 0x9000:	/* PCIe2 */
		if (host_agent == 0 || host_agent == 1 ||
			host_agent == 4 || host_agent == 5)
			return 0;
		break;
	case 0xb000:	/* PCIe3 */
		if (host_agent == 0 || host_agent == 2 ||
			host_agent == 4 || host_agent == 6)
			return 0;
		break;
	default:
		return 1;
	}
	return 1;
}

/*Added by suipl 2013.2.25 for JH1022
 * For JH1022 1WAC boards, do any additional late init after all drivers are up.
 */
static int __init p1022rdk_1wac_init(void)
{
    if (!of_machine_is_compatible("fsl,P1022DS,1WAC"))
        return 0;

    /* 
     * Write PORTSC (status/control) register to configure PHY in shutdown mode 
     *
     * TODO: this is currently removed - the USB driver cannot currently handle
     * coming out of deep sleep when the PHY is shutdown.  Proper handling
     * would be for the driver to shutdown/resume the PHY in its
     * suspend/resume calls.
     */
    if (0) {
        u32 __iomem *usb;

        usb = ioremap(get_immrbase() + 0x22000, sizeof(u32));
        if (!usb) {
            printk(KERN_ERR "Err: can't map IO register!\n");
            return 0;
        }
        usb[0x184/sizeof(u32)] = 0x00108096;
        iounmap(usb);
    }

    return 0;
}
late_initcall(p1022rdk_1wac_init);

/*
 * Setup the architecture
 */
#ifdef CONFIG_SMP
extern void __init mpc85xx_smp_init(void);
#endif
static void __init p1022_ds_setup_arch(void)
{
#ifdef CONFIG_PCI
	struct device_node *np;
	struct pci_controller *hose;
	u32 host_agent;
#endif
	dma_addr_t max = 0xffffffff;

	if (ppc_md.progress)
		ppc_md.progress("p1022_ds_setup_arch()", 0);

#ifdef CONFIG_PCI
	host_agent = get_p1022ds_host_agent();
	for_each_node_by_type(np, "pci") {
		if (of_device_is_compatible(np, "fsl,mpc8540-pci") ||
		    of_device_is_compatible(np, "fsl,mpc8548-pcie") ||
		    of_device_is_compatible(np, "fsl,p1022-pcie")) {
			struct resource rsrc;
			of_address_to_resource(np, 0, &rsrc);

			if (!p1022ds_pci_is_host(host_agent, rsrc.start))
				continue;
			if ((rsrc.start & 0xfffff) == 0x8000)
				fsl_add_bridge(np, 1);
			else
				fsl_add_bridge(np, 0);

			hose = pci_find_hose_for_OF_device(np);
			max = min(max, hose->dma_window_base_cur +
					hose->dma_window_size);
		}
	}

#endif

#if defined(CONFIG_FB_FSL_DIU) || defined(CONFIG_FB_FSL_DIU_MODULE)
	diu_ops.get_pixel_format	= p1022ds_get_pixel_format;
	diu_ops.set_gamma_table		= p1022ds_set_gamma_table;
	diu_ops.set_monitor_port	= p1022ds_set_monitor_port;
	diu_ops.set_pixel_clock		= p1022ds_set_pixel_clock;
	diu_ops.show_monitor_port	= p1022ds_show_monitor_port;
	diu_ops.set_sysfs_monitor_port	= p1022ds_set_sysfs_monitor_port;
#endif

#ifdef CONFIG_SMP
	mpc85xx_smp_init();
#endif

#ifdef CONFIG_SWIOTLB
	if (memblock_end_of_DRAM() > max) {
		ppc_swiotlb_enable = 1;
		set_pci_dma_ops(&swiotlb_dma_ops);
		ppc_md.pci_dma_dev_setup = pci_dma_dev_setup_swiotlb;
	}
#endif

	printk("JH1022 board from Freescale Semiconductor\n"); }

static struct of_device_id __initdata p1022_ds_ids[] = {
	{ .type = "soc", },
	{ .compatible = "soc", },
	{ .compatible = "simple-bus", },
	{ .compatible = "gianfar", },
	{},
};

static int __init p1022_ds_publish_devices(void)
{
	return of_platform_bus_probe(NULL, p1022_ds_ids, NULL);
}
machine_device_initcall(p1022_ds, p1022_ds_publish_devices);

machine_arch_initcall(p1022_ds, swiotlb_setup_bus_notifier);

/*
 * Called very early, device-tree isn't unflattened
 */
static int __init p1022_ds_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,p1022ds");
}

define_machine(p1022_ds) {
	.name			= "P1022 DS",
	.probe			= p1022_ds_probe,
	.setup_arch		= p1022_ds_setup_arch,
	.init_IRQ		= p1022_ds_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};
