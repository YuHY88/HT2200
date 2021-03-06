/*
 * Copyright (C) 2007-2009 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author: Tony Li <tony.li@freescale.com>
 *
 * Description:
 * The Utopia POS Controller function
 *
 * Changelog:
 * Mar 19 2008	Dave Liu <daveliu@freescale.com>
 *              Jiang Bo <tanya.jiang@freescale.com>
 * - Fixed bug for cmxupcr register setting and upc rx/tx clock setting
 * - Misc code cleanup.
 *
 * June 2009    Liu Yu <yu.liu@freescale.com>
 * - cleanup
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <asm/prom.h>
#include <asm/upc.h>

#include "upc.h"

struct upc_info upc_info[UPC_MAX_NUM];
struct upc_private upc_priv[UPC_MAX_NUM];

struct upc_private *get_upc_bus(int upc_num)
{
	if ((upc_num < 0) || (upc_num > UPC_MAX_NUM))
		return NULL;

	return &upc_priv[upc_num];
}

int upc_set_qe_mux_clock(int upc_num, const char *clk, enum clk_dir mode)
{
	u32 clock_bits, clock_mask;
	struct clock_map *map;
	int source, i;
       /* upc 1 rx tx and internal clock map */
	struct clock_map  upc1_clk [16] = {
		{CLK_RTX, "brg5", 1},
		{CLK_RTX, "brg6", 2},
		{CLK_RTX, "brg7", 3},
		{CLK_RTX, "brg8", 4},
		{CLK_RTX, "clk13", 5},
		{CLK_RTX, "clk14", 6},
		{CLK_RTX, "clk15", 7},
		{CLK_RTX, "clk16", 8},
		{CLK_RTX, "clk19", 9},
		{CLK_RTX, "clk20", 10},
		{CLK_INTERNAL, "none", 0},
		{CLK_INTERNAL, "brg3", 1},
		{CLK_INTERNAL, "brg4", 2},
		{CLK_INTERNAL, "clk17", 5},
		{CLK_INTERNAL, "clk18", 3},
		{CLK_INTERNAL, "clk19", 4},
	};
	/* upc 2 rx tx and internal clock map */
	struct clock_map  upc2_clk[16] = {
		{CLK_RTX, "brg13", 1},
		{CLK_RTX, "brg14", 2},
		{CLK_RTX, "brg15", 3},
		{CLK_RTX, "brg16", 4},
		{CLK_RTX, "clk5", 5},
		{CLK_RTX, "clk6", 6},
		{CLK_RTX, "clk7", 7},
		{CLK_RTX, "clk8", 8},
		{CLK_RTX, "clk19", 9},
		{CLK_RTX, "clk20", 10},
		{CLK_INTERNAL, "none", 0},
		{CLK_INTERNAL, "brg3", 1},
		{CLK_INTERNAL, "brg4", 2},
		{CLK_INTERNAL, "clk18", 3},
		{CLK_INTERNAL, "clk19", 4},
		{CLK_INTERNAL, "clk20", 5},
	};

	source = -1;

	if ((upc_num < 0) || (upc_num > UPC_MAX_NUM))
		return -EINVAL;

	if (upc_num == 0)
		map = upc1_clk;
	else
		map = upc2_clk;

	for (i = 0; i < 16; i++) {
		if ((map[i].dir & mode) &&
				(strcasecmp(map[i].name, clk) == 0)) {
			source = map[i].value;
			break;
		}
	}

	if (source == -1) {
		printk(KERN_ERR"upc_set_qe_mux_clock: \
			Bad combination of clock and UPC.\n");
		return -ENOENT;
	}

	clock_bits = source;
	clock_mask = 0xF;
	if (mode == CLK_RX) {
		clock_bits <<= QE_CMXUPCR_RCS_SHIFT;
		clock_mask <<= QE_CMXUPCR_RCS_SHIFT;
	} else if (mode == CLK_TX) {
		clock_bits <<= QE_CMXUPCR_TCS_SHIFT;
		clock_mask <<= QE_CMXUPCR_TCS_SHIFT;
	} else
		clock_mask = QE_CMXUPCR_RCS_MASK;

	/* UPC 1 resides at high half of register */
	if (upc_num == 0) {
		clock_bits <<= 16;
		clock_mask <<= 16;
	}

	clrsetbits_be32(&qe_immr->qmx.cmxupcr, clock_mask, clock_bits);

	return 0;
}

int upc_slot_ep_mask(int slot_num, int ep_num,
			enum comm_dir dir, struct upc *upc_reg)
{
	u32 value;
	u32 *reg;

	reg = (&upc_reg->updc1 + slot_num);
	value = in_be32(reg);

	if (((dir & COMM_DIR_TX) && !(value & UPDC_PE_TX)) ||
		((dir & COMM_DIR_RX) && !(value & UPDC_PE_RX)))
		return -EPERM;

	reg = (&upc_reg->uper1 + slot_num);
	clrbits32(reg, 1 << (31 - ep_num));

	return 0;
}

int upc_slot_ep_umask(int slot_num, int ep_num,
			enum comm_dir dir, struct upc *upc_reg)
{
	u32 value;
	u32 *reg;

	reg = (&upc_reg->updc1 + slot_num);
	value = in_be32(reg);

	if (((dir & COMM_DIR_TX) && !(value & UPDC_PE_TX)) ||
		((dir & COMM_DIR_RX) && !(value & UPDC_PE_RX)))
		return -EPERM;

	reg = (&upc_reg->uper1 + slot_num);
	setbits32(reg, 1 << (31 - ep_num));

	return 0;
}

int upc_slot_ep_rec_pri_sel(int slot_num, int ep_num,
				int rpp, struct upc *upc_regs)
{
	u32 value;
	u32 *reg;

	reg = (&upc_regs->updrp1 + slot_num);
	value = in_be32(reg);
	if (rpp == 1)
		setbits32(reg, 1 << (UPC_SLOT_EP_MAX_NUM - 1 - ep_num));
	else if (rpp == 0)
		clrbits32(reg, 1 << (UPC_SLOT_EP_MAX_NUM - 1 - ep_num));

	return 0;
}

int upc_slot_ep_rate_sel(int slot_num, int ep_num,
				int sub_rate, struct upc *upc_regs)
{
	u32 shift;
	u32 *reg;

	if (ep_num < 15) {
		reg = &upc_regs->updrs1_h + slot_num * 2 + 1;
		shift = 15 - ep_num;
	} else {
		reg = &upc_regs->updrs1_h + slot_num * 2;
		shift = 31 - ep_num;
	}

	setbits32(reg, (sub_rate & 0x3) << (shift * 2));

	return 0;
}

int upc_slot_rate_conf(int slot_num, struct upc_slot_rate_info *rate,
			struct upc *upc_regs)
{
	int i;
	u32 value;
	u16 *reg;

	if (rate->tirec & ~0xF) {
		printk(KERN_WARNING"%s The tirec value is out of bound. \
				reducd it form 0x%x to 0x%x\n",	__FUNCTION__,
				rate->tirec, rate->tirec & 0xF);
		rate->tirec = rate->tirec & 0xF;
	}
	reg = (&upc_regs->uprp1 + slot_num);
	value = rate->pre << 8 | rate->tirec;
	out_be16(reg, value);

	for (i = 0; i < 4; i++) {
		if (rate->sub_en[i]) {
			value = (rate->tirr[i] & UPTIRR_TIRR) | UPTIRR_EN;
			reg = (&upc_regs->uptirr1_0 + (slot_num * 4) + i);
			out_be16(reg, value);
		}
	}

	return 0;
}

int _ucc_attach_upc_tx_slot(int ucc_num, struct upc_slot_tx *ust,
			struct upc_private *upc_priv)
{
	struct upc *upc_regs;
	u32 *reg;
	u32 value;
	int shift;

	if ((ust->slot < 0) || (ust->slot > UPC_SLOT_MAX_NUM - 1)) {
		printk(KERN_ERR"%s: illegal UPC Tx device number\n",
			 __FUNCTION__);
		return -EINVAL;
	}

	shift = (3 - ucc_num / 2) * 8;
	upc_regs = upc_priv->upc_regs;

	/* UPUC */
	value = in_be32(&upc_regs->upuc);
	value &= ~(UPUC_ALL << shift);
	if (ust->tmp) {
		value |= (UPUC_TMP << shift);
	}
	if (ust->tsp) {
		value |= (UPUC_TSP << shift);
	}
	if (ust->tb2b) {
		value |= (UPUC_TB2B << shift);
	}
	out_be32(&upc_regs->upuc, value);

	/* UPDC Tx */
	reg = &upc_regs->updc1 + ust->slot;
	value = in_be32(reg);
	value &= ~UPDC_TXUCC;
	value |= ((ucc_num / 2) << (ffs(UPDC_TXUCC) - 1)) & UPDC_TXUCC;
	value &= ~UPDC_TEHS;
	value |= (ust->tehs << (ffs(UPDC_TEHS) - 1)) & UPDC_TEHS;
	value &= ~UPDC_TUDC;
	value |= (ust->tudc << (ffs(UPDC_TUDC) - 1)) & UPDC_TUDC;
	value &= ~UPDC_TXPW;
	value |= (ust->txpw << (ffs(UPDC_TXPW) - 1)) & UPDC_TXPW;
	value &= ~UPDC_TPM;
	value |= (ust->tpm << (ffs(UPDC_TPM) - 1)) & UPDC_TPM;
	value |= UPDC_TXENB;

	out_be32(reg, value);

	return 0;
}

int ucc_attach_upc_tx_slot(int ucc_num, struct upc_slot_tx *ust)
{
	if ((ucc_num < 0) || (ucc_num > 7))
		return -EINVAL;

	return _ucc_attach_upc_tx_slot(ucc_num, ust,
			&upc_priv[ ucc_num % 2 ? 1 : 0]);
}

int _ucc_detach_upc_tx_slot(int ucc_num, struct upc_slot_tx *ust,
				struct upc_private *upc_priv)
{
	u32 *reg;

	if ((ust->slot < 0) || ust->slot > UPC_SLOT_MAX_NUM - 1) {
		printk(KERN_ERR"%s: illegal UPC Tx device number\n",
			__FUNCTION__);
		return -EINVAL;
	}

	reg = &upc_priv->upc_regs->updc1 + ust->slot;
	clrbits32(reg, UPDC_TXENB);

	return 0;
}

int ucc_detach_upc_tx_slot(int ucc_num, struct upc_slot_tx *ust)
{
	if ((ucc_num > 0) || (ucc_num < 7))
		return -EINVAL;

	return _ucc_detach_upc_tx_slot(ucc_num, ust,
			&upc_priv[ucc_num % 2 ? 1 : 0]);
}

int _ucc_attach_upc_rx_slot(int ucc_num, struct upc_slot_rx *usr,
				struct upc_private *upc_priv)
{
	struct upc *upc_regs;
	u32 *reg;
	u32 value;

	if ((usr->slot < 0) || (usr->slot > UPC_SLOT_MAX_NUM - 1)) {
		printk(KERN_ERR"%s: illegal UPC Tx device number\n",
			__FUNCTION__);
		return -EINVAL;
	}

	upc_regs = upc_priv->upc_regs;
	/* UPDC Rx */
	reg = &upc_regs->updc1 + usr->slot;
	value = in_be32(reg);
	value &= ~UPDC_RXUCC;
	value |= ((ucc_num / 2) << (ffs(UPDC_RXUCC) - 1)) & UPDC_RXUCC;
	value &= ~UPDC_REHS;
	value |= (usr->rehs << (ffs(UPDC_REHS) - 1)) & UPDC_REHS;
	value &= ~UPDC_RMP;
	value |= (usr->rmp << (ffs(UPDC_RMP) - 1)) & UPDC_RMP;
	value &= ~UPDC_RB2B;
	value |= (usr->rb2b << (ffs(UPDC_RB2B) - 1)) & UPDC_RB2B;
	value &= ~UPDC_RUDC;
	value |= (usr->rudc << (ffs(UPDC_RUDC) - 1)) & UPDC_RUDC;
	value &= ~UPDC_RXPW;
	value |= (usr->rxpw << (ffs(UPDC_RXPW) - 1)) & UPDC_RXPW;
	value &= ~UPDC_RXP;
	value |= (usr->rxp << (ffs(UPDC_RXP) - 1)) & UPDC_RXP;
	value |= UPDC_RXENB;
	out_be32(reg, value);

	return 0;
}

int ucc_attach_upc_rx_slot(int ucc_num, struct upc_slot_rx *usr)
{
	if ((ucc_num < 0) || (ucc_num > 7))
		return -EINVAL;

	return 	_ucc_attach_upc_rx_slot(ucc_num, usr,
			&upc_priv[ ucc_num % 2 ? 1 : 0]);

}

int _ucc_detach_upc_rx_slot(int ucc_num, struct upc_slot_rx *usr,
				struct upc_private *upc_priv)
{
	u32 *reg;

	if ((usr->slot < 0) || usr->slot > UPC_SLOT_MAX_NUM - 1) {
		printk(KERN_ERR"%s: illegal UPC Rx device number\n",
			__FUNCTION__);
		return -EINVAL;
	}

	reg = &upc_priv->upc_regs->updc1 + usr->slot;
	clrbits32(reg, UPDC_RXENB);

	return 0;
}

int ucc_detach_upc_rx_slot(int ucc_num, struct upc_slot_rx *usr)
{
	if ((ucc_num > 0) || (ucc_num < 7))
		return -EINVAL;

	return _ucc_detach_upc_rx_slot(ucc_num, usr,
			&upc_priv[ ucc_num % 2 ? 1 : 0]);
}

int upc_slot_init(struct upc_slot_info *us_info, struct upc *upc_regs)
{
	void *reg;
	u32 value;
	int i;

	value = 0;
	if (us_info->icd)
		value |= UPDC_ICD;
	if (us_info->pe)
		value |= (us_info->pe << (ffs(UPDC_PE) - 1)) & UPDC_PE;
	if (us_info->heci)
		value |= UPDC_HECI;
	if (us_info->hecc)
		value |= UPDC_HECC;
	if (us_info->cos)
		value |= UPDC_COS;

	reg = &upc_regs->updc1 + us_info->upc_slot_num;
	out_be32(reg, value);

	upc_slot_rate_conf(us_info->upc_slot_num, &us_info->rate, upc_regs);

	for (i = 0; i < us_info->ep_num; i++) {
		upc_slot_ep_rate_sel(us_info->upc_slot_num, i, 0, upc_regs);
		upc_slot_ep_rec_pri_sel(us_info->upc_slot_num, i, 0, upc_regs);
		upc_slot_ep_umask(us_info->upc_slot_num, i,
					COMM_DIR_RX_AND_TX, upc_regs);
	}

	return 0;
}

int upc_ul_init(struct upc_info *upc_info, struct upc_private *upc_priv)
{
	struct upc *upc_regs;
	int i, j;
	u32 value;

	upc_regs = ioremap(upc_info->regs, sizeof(struct upc));
	if (!upc_regs) {
		printk(KERN_ERR "%s: Cannot map UPC regsiters", __FUNCTION__);
		return -ENOMEM;
	}

	if (upc_set_qe_mux_clock(upc_info->upc_num,
			upc_info->tx_clock, CLK_TX)) {
		printk(KERN_ERR"%s: illegal value for tx clock\n",
				__FUNCTION__);
		return -EINVAL;
	}
	if (upc_set_qe_mux_clock(upc_info->upc_num,
		upc_info->rx_clock, CLK_RX)) {
		printk(KERN_ERR"%s: illegal value for rx clock\n",
				__FUNCTION__);
		printk(KERN_ERR"upc%d: clock num: %s\n",
				upc_info->upc_num, upc_info->rx_clock);
		return -EINVAL;
	}
	if (upc_set_qe_mux_clock(upc_info->upc_num,
		upc_info->internal_clock, CLK_INTERNAL)) {
		printk(KERN_ERR"%s: illegal value for internal clock\n",
			__FUNCTION__);
		return -EINVAL;
	}

	value = 0;
	if (upc_info->tms)
		value |= UPGCR_TMS;
	else if (upc_info->rms)
		value |= UPGCR_RMS;
	if (upc_info->addr)
		value |= UPGCR_ADDR;
	if (upc_info->diag)
		value |= UPGCR_DIAG;

	out_be32(&upc_regs->upgcr, value);

	value = (upc_info->uplpat << UPLPA_TX_LAST_PHY_SHIFT) |
			(upc_info->uplpar << UPLPA_RX_LAST_PHY_SHIFT);
	out_be32(&upc_regs->uplpa, value);

	out_be32(&upc_regs->uphec, upc_info->uphec);

	value = 0;
	out_be32(&upc_regs->upuc, value);

	upc_priv->upc_regs = upc_regs;
	upc_priv->upc_info = upc_info;

	for (i = j = 0; i < UPC_SLOT_MAX_NUM; i++) {
		if (upc_info->us_info[i] == NULL) {
			printk(KERN_WARNING"%s: Fail to init UPC device %d\n",
					__FUNCTION__, i);
			j++;
			continue;
		}
		upc_slot_init(upc_info->us_info[i], upc_priv->upc_regs);
	}
	if ( j == i)
		return -ENODEV;

	return 0;
}

int upc_init(void)
{
	int i, j, ret;
	struct device_node *np, *slot;
	const unsigned int *prop;
	struct resource res;

	ret = 0;
	for_each_compatible_node(np, NULL, "fsl,qe-upc") {
		prop = of_get_property(np, "device-id", NULL);
		if ((*prop < 0) || (*prop > UPC_MAX_NUM )) {
			printk(KERN_ERR"%s: illegal UPC number", __FUNCTION__);
			return -ENODEV;
		}
		i = *prop - 1;
		upc_info[i].upc_num = i;

		if (of_get_property(np, "fsl,utopia", NULL) != NULL)
			upc_info[i].mode = UTOPIA;

		if (of_address_to_resource(np, 0, &res))
			return -EINVAL;
		upc_info[i].regs = (u32)res.start;
		prop = (const unsigned int *)
			of_get_property(np, "fsl,tx-clock", NULL);
		upc_info[i].tx_clock = (const char *)prop;
		prop = (const unsigned int *)
			of_get_property(np, "fsl,rx-clock", NULL);
		upc_info[i].rx_clock = (const char *)prop;
		prop = (const unsigned int *)
			of_get_property(np, "fsl,internal-clock", NULL);
		upc_info[i].internal_clock = (const char *)prop;

		prop = of_get_property(np, "fsl,tx-slave", NULL);
		upc_info[i].tms = *prop;
		prop = of_get_property(np, "fsl,rx-slave", NULL);
		upc_info[i].rms = *prop;
		prop = of_get_property(np, "fsl,address-mode", NULL);
		upc_info[i].addr = *prop;

		if (upc_info[i].tms) { /* Tx slave mode */
			prop = of_get_property(np, "fsl,tx-address", NULL);
			upc_info[i].uplpat = *prop;
		} else if (upc_info[i].addr) /* Tx master mode 6-bit address */
			upc_info[i].uplpat = 15;
		else	/* Tx master mode 5-bit address */
			upc_info[i].uplpat = 31;

		if (upc_info[i].rms) { /* Rx slave mode */
			prop = of_get_property(np, "fsl,rx-address", NULL);
			upc_info[i].uplpar = *prop;
		} else if (upc_info[i].addr) /* Rx master mode 6-bit address */
			upc_info[i].uplpar = 15;
		else	/* Rx master mode 5-bit address */
			upc_info[i].uplpar = 31;

		upc_info[i].uphec = 0x5A5A0000;

		for (slot = np; (slot =
			of_find_compatible_node(slot, NULL, "fsl,qe-upc-slot"))
								 != NULL;) {
			struct upc_slot_info *upc_slot;
			upc_slot = kzalloc(sizeof(struct upc_slot_info),
					GFP_KERNEL);
			if (upc_slot == NULL)
				continue;

			prop = of_get_property(slot, "device-id", NULL);
			upc_slot->upc_slot_num = *prop - 1;
			prop = of_get_property(slot, "fsl,end-point", NULL);
			upc_slot->ep_num = *prop;

			upc_slot->icd = 0;
			upc_slot->pe = 3;
			upc_slot->heci = 0;
			upc_slot->hecc = 0;
			upc_slot->cos = 0;
			upc_slot->rate.pre = 0;
			upc_slot->rate.tirec = 0;
			for (j = 0; j < 4; j++) {
				upc_slot->rate.sub_en[j] = 0;
				upc_slot->rate.tirr[j] = 0;
			}

			upc_info[i].us_info[upc_slot->upc_slot_num] = upc_slot;
		}

		/* par_io_of_config(np); It has been done by setup_arch()*/
		if (upc_info->mode == UTOPIA) {
			ret = upc_ul_init(upc_info, &upc_priv[i]);
			if (ret)
				printk(KERN_ERR"%s UPC bus %d error\n",
					__FUNCTION__, upc_info[i].upc_num);
		} else
			printk(KERN_ERR"%s: Does not support protocols other \
				than UTOPIA\n", __FUNCTION__);
	}
	return ret;
}
EXPORT_SYMBOL(upc_init);

void _upc_dump(struct upc *upc)
{
	printk(KERN_DEBUG"upc at 0x%p\n", upc);
	printk(KERN_DEBUG"\tupgcr: 0x%x at %p\n", upc->upgcr, &upc->upgcr);
	printk(KERN_DEBUG"\tuplpa: 0x%x at %p\n", upc->uplpa, &upc->uplpa);
	printk(KERN_DEBUG"\tuphec: 0x%x at %p\n", upc->uphec, &upc->uphec);
	printk(KERN_DEBUG"\tupuc: 0x%x at %p\n", upc->upuc, &upc->upuc);
	printk(KERN_DEBUG"\tupdc1: 0x%x at %p\n", upc->updc1, &upc->updc1);
	printk(KERN_DEBUG"\tupdrs1_h: 0x%x at %p\n",
				upc->updrs1_h, &upc->updrs1_h);
	printk(KERN_DEBUG"\tupdrs1_l: 0x%x at %p\n",
				upc->updrs1_l, &upc->updrs1_l);
	printk(KERN_DEBUG"\tupdrp1: 0x%x at %p\n", upc->updrp1, &upc->updrp1);
	printk(KERN_DEBUG"\tupde1: 0x%x at %p\n", upc->upde1, &upc->upde1);
	printk(KERN_DEBUG"\tuprp1: 0x%x at %p\n", upc->uprp1, &upc->uprp1);
	printk(KERN_DEBUG"\tuptirr1_0: 0x%x at %p\n",
		upc->uptirr1_0, &upc->uptirr1_0);
	printk(KERN_DEBUG"\tuptirr1_1: 0x%x at %p\n",
		upc->uptirr1_1, &upc->uptirr1_1);
	printk(KERN_DEBUG"\tuptirr1_2: 0x%x at %p\n",
		upc->uptirr1_2, &upc->uptirr1_2);
	printk(KERN_DEBUG"\tuptirr1_3: 0x%x at %p\n",
		upc->uptirr1_3, &upc->uptirr1_3);
	printk(KERN_DEBUG"\tuper1: 0x%x at %p\n", upc->uper1, &upc->uper1);
}

void upc_dump(int upc_num)
{
	_upc_dump(upc_priv[upc_num].upc_regs);
}
EXPORT_SYMBOL(upc_dump);
