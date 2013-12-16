/*
 * Atmel AT91 AIC (Advanced Interrupt Controller) driver
 *
 *  Copyright (C) 2004 SAN People
 *  Copyright (C) 2004 ATMEL
 *  Copyright (C) Rick Bronson
 *  Copyright (C) 2013 Boris BREZILLON <b.brezillon@overkiz.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/bitmap.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

#include "irqchip.h"

/* Number of irq lines managed by AIC */
#define NR_AIC_IRQS	32
#define NR_AIC5_IRQS	128

#define AT91_AIC5_SSR		0x0
#define		AT91_AIC5_INTSEL_MSK	(0x7f << 0)

#define AT91_AIC_IRQ_MIN_PRIORITY	0
#define AT91_AIC_IRQ_MAX_PRIORITY	7

#define AT91_AIC_SMR(n)		((n) * 4)
#define AT91_AIC5_SMR		0x4
#define		AT91_AIC_PRIOR		(7 << 0)
#define		AT91_AIC_SRCTYPE	(3 << 5)
#define			AT91_AIC_SRCTYPE_LOW		(0 << 5)
#define			AT91_AIC_SRCTYPE_FALLING	(1 << 5)
#define			AT91_AIC_SRCTYPE_HIGH		(2 << 5)
#define			AT91_AIC_SRCTYPE_RISING		(3 << 5)

#define AT91_AIC_SVR(n)		(0x80 + ((n) * 4))
#define AT91_AIC5_SVR		0x8
#define AT91_AIC_IVR		0x100
#define AT91_AIC5_IVR		0x10
#define AT91_AIC_FVR		0x104
#define AT91_AIC5_FVR		0x14
#define AT91_AIC_ISR		0x108
#define AT91_AIC5_ISR		0x18
#define		AT91_AIC_IRQID		(0x1f << 0)

#define AT91_AIC_IPR		0x10c
#define AT91_AIC5_IPR0		0x20
#define AT91_AIC5_IPR1		0x24
#define AT91_AIC5_IPR2		0x28
#define AT91_AIC5_IPR3		0x2c
#define AT91_AIC_IMR		0x110
#define AT91_AIC5_IMR		0x30
#define AT91_AIC_CISR		0x114
#define AT91_AIC5_CISR		0x34
#define		AT91_AIC_NFIQ		(1 << 0)
#define		AT91_AIC_NIRQ		(1 << 1)

#define AT91_AIC_IECR		0x120
#define AT91_AIC5_IECR		0x40
#define AT91_AIC_IDCR		0x124
#define AT91_AIC5_IDCR		0x44
#define AT91_AIC_ICCR		0x128
#define AT91_AIC5_ICCR		0x48
#define AT91_AIC_ISCR		0x12c
#define AT91_AIC5_ISCR		0x4c
#define AT91_AIC_EOICR		0x130
#define AT91_AIC5_EOICR		0x38
#define AT91_AIC_SPU		0x134
#define AT91_AIC5_SPU		0x3c
#define AT91_AIC_DCR		0x138
#define AT91_AIC5_DCR		0x6c
#define		AT91_AIC_DCR_PROT	(1 << 0)
#define		AT91_AIC_DCR_GMSK	(1 << 1)

#define AT91_AIC_FFER		0x140
#define AT91_AIC5_FFER		0x50
#define AT91_AIC_FFDR		0x144
#define AT91_AIC5_FFDR		0x54
#define AT91_AIC_FFSR		0x148
#define AT91_AIC5_FFSR		0x58

enum aic_mux_irq_type {
	AIC_MUX_1REG_IRQ,
	AIC_MUX_3REG_IRQ,
};

struct aic_mux_irq {
	struct list_head node;
	enum aic_mux_irq_type type;
	void __iomem *base;
	u32 offset;
	u32 mask;
};

struct aic_chip_data {
	u32 ext_irqs;
	struct list_head mux[32];
};

static struct irq_domain *aic_domain;

static asmlinkage void __exception_irq_entry
aic_handle(struct pt_regs *regs)
{
	struct irq_domain_chip_generic *dgc = aic_domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];
	u32 irqnr;
	u32 irqstat;

	irqnr = irq_reg_readl(gc->reg_base + AT91_AIC_IVR);
	irqstat = irq_reg_readl(gc->reg_base + AT91_AIC_ISR);

	irqnr = irq_find_mapping(aic_domain, irqnr);

	if (!irqstat)
		irq_reg_writel(0, gc->reg_base + AT91_AIC_EOICR);
	else
		handle_IRQ(irqnr, regs);
}

static asmlinkage void __exception_irq_entry
aic5_handle(struct pt_regs *regs)
{
	struct irq_domain_chip_generic *dgc = aic_domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];
	u32 irqnr;
	u32 irqstat;

	irqnr = irq_reg_readl(gc->reg_base + AT91_AIC5_IVR);
	irqstat = irq_reg_readl(gc->reg_base + AT91_AIC5_ISR);

	irqnr = irq_find_mapping(aic_domain, irqnr);

	if (!irqstat)
		irq_reg_writel(0, gc->reg_base + AT91_AIC5_EOICR);
	else
		handle_IRQ(irqnr, regs);
}

static void aic5_mask(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];

	/* Disable interrupt on AIC5 */
	irq_gc_lock(gc);
	irq_reg_writel(d->hwirq, gc->reg_base + AT91_AIC5_SSR);
	irq_reg_writel(1, gc->reg_base + AT91_AIC5_IDCR);
	gc->mask_cache &= ~d->mask;
	irq_gc_unlock(gc);
}

static void aic5_unmask(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];

	/* Enable interrupt on AIC5 */
	irq_gc_lock(gc);
	irq_reg_writel(d->hwirq, gc->reg_base + AT91_AIC5_SSR);
	irq_reg_writel(1, gc->reg_base + AT91_AIC5_IECR);
	gc->mask_cache |= d->mask;
	irq_gc_unlock(gc);
}

static int aic_retrigger(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);

	/* Enable interrupt on AIC5 */
	irq_gc_lock(gc);
	irq_reg_writel(d->mask, gc->reg_base + AT91_AIC_ISCR);
	irq_gc_unlock(gc);

	return 0;
}

static int aic5_retrigger(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];

	/* Enable interrupt on AIC5 */
	irq_gc_lock(gc);
	irq_reg_writel(d->hwirq, gc->reg_base + AT91_AIC5_SSR);
	irq_reg_writel(1, gc->reg_base + AT91_AIC5_ISCR);
	irq_gc_unlock(gc);

	return 0;
}

static int aic_to_srctype(struct irq_data *d, unsigned type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct aic_chip_data *aic = gc->private;

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
		return AT91_AIC_SRCTYPE_HIGH;
	case IRQ_TYPE_EDGE_RISING:
		return AT91_AIC_SRCTYPE_RISING;
	case IRQ_TYPE_LEVEL_LOW:
		if (d->mask & aic->ext_irqs)
			return AT91_AIC_SRCTYPE_LOW;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		if (d->mask & aic->ext_irqs)
			return AT91_AIC_SRCTYPE_FALLING;
		break;
	default:
		break;
	}

	return -EINVAL;
}

static int aic_set_type(struct irq_data *d, unsigned type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	unsigned int smr;
	int srctype;

	srctype = aic_to_srctype(d, type);
	if (srctype < 0)
		return srctype;

	smr = irq_reg_readl(gc->reg_base + AT91_AIC_SMR(d->hwirq)) &
	      ~AT91_AIC_SRCTYPE;
	irq_reg_writel(smr | srctype, gc->reg_base + AT91_AIC_SMR(d->hwirq));

	return 0;
}

static int aic5_set_type(struct irq_data *d, unsigned type)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];
	unsigned int smr;
	int srctype;

	srctype = aic_to_srctype(d, type);
	if (srctype < 0)
		return srctype;

	irq_gc_lock(gc);
	irq_reg_writel(d->hwirq, gc->reg_base + AT91_AIC5_SSR);
	smr = irq_reg_readl(gc->reg_base + AT91_AIC5_SMR) & ~AT91_AIC_SRCTYPE;
	irq_reg_writel(smr | srctype, gc->reg_base + AT91_AIC5_SMR);
	irq_gc_unlock(gc);

	return 0;
}

static void aic_mux_disable_irqs(struct list_head *mux_list)
{
	struct aic_mux_irq *irq;

	list_for_each_entry(irq, mux_list, node) {
		if (irq->type == AIC_MUX_1REG_IRQ)
			writel(readl(irq->base + irq->offset) & ~irq->mask,
			       irq->base + irq->offset);
		else
			writel(irq->mask, irq->base + irq->offset);
	}
}

static void aic_shutdown(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	struct aic_chip_data *aic = gc->private;
	int idx = d->hwirq % 32;

	aic_mux_disable_irqs(&aic->mux[idx]);
	ct->chip.irq_mask(d);
}

#ifdef CONFIG_PM
static void aic_suspend(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);

	irq_gc_lock(gc);
	irq_reg_writel(gc->mask_cache, gc->reg_base + AT91_AIC_IDCR);
	irq_reg_writel(gc->wake_active, gc->reg_base + AT91_AIC_IECR);
	irq_gc_unlock(gc);
}

static void aic5_suspend(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *bgc = dgc->gc[0];
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	int i;
	u32 mask;

	irq_gc_lock(bgc);
	for (i = 0; i < dgc->irqs_per_chip; i++) {
		mask = 1 << i;
		if ((mask & gc->mask_cache) == (mask & gc->wake_active))
			continue;

		irq_reg_writel(i + gc->irq_base,
			       bgc->reg_base + AT91_AIC5_SSR);
		if (mask & gc->wake_active)
			irq_reg_writel(1, bgc->reg_base + AT91_AIC5_IECR);
		else
			irq_reg_writel(1, bgc->reg_base + AT91_AIC5_IDCR);
	}
	irq_gc_unlock(bgc);
}

static void aic_resume(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);

	irq_gc_lock(gc);
	irq_reg_writel(gc->wake_active, gc->reg_base + AT91_AIC_IDCR);
	irq_reg_writel(gc->mask_cache, gc->reg_base + AT91_AIC_IECR);
	irq_gc_unlock(gc);
}

static void aic5_resume(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *bgc = dgc->gc[0];
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	int i;
	u32 mask;

	irq_gc_lock(bgc);
	for (i = 0; i < dgc->irqs_per_chip; i++) {
		mask = 1 << i;
		if ((mask & gc->mask_cache) == (mask & gc->wake_active))
			continue;

		irq_reg_writel(i + gc->irq_base,
			       bgc->reg_base + AT91_AIC5_SSR);
		if (mask & gc->mask_cache)
			irq_reg_writel(1, bgc->reg_base + AT91_AIC5_IECR);
		else
			irq_reg_writel(1, bgc->reg_base + AT91_AIC5_IDCR);
	}
	irq_gc_unlock(bgc);
}

static void aic_pm_shutdown(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);

	irq_gc_lock(gc);
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC_IDCR);
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC_ICCR);
	irq_gc_unlock(gc);
}

static void aic5_pm_shutdown(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_domain_chip_generic *dgc = domain->gc;
	struct irq_chip_generic *bgc = dgc->gc[0];
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	int i;

	irq_gc_lock(bgc);
	for (i = 0; i < dgc->irqs_per_chip; i++) {
		irq_reg_writel(i + gc->irq_base,
			       bgc->reg_base + AT91_AIC5_SSR);
		irq_reg_writel(1, bgc->reg_base + AT91_AIC5_IDCR);
		irq_reg_writel(1, bgc->reg_base + AT91_AIC5_ICCR);
	}
	irq_gc_unlock(bgc);
}
#else
#define aic_suspend		NULL
#define aic5_suspend		NULL
#define aic_resume		NULL
#define aic5_resume		NULL
#define aic_pm_shutdown		NULL
#define aic5_pm_shutdown	NULL
#endif /* CONFIG_PM */

static void __init aic_mux_hw_init(struct irq_domain *domain)
{
	struct aic_chip_data *aic = domain->host_data;
	int i;

	for (i = 0; i < domain->revmap_size; i++)
		aic_mux_disable_irqs(&aic[i / 32].mux[i % 32]);
}

static void __init aic_hw_init(struct irq_domain *domain)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(domain, 0);
	int i;

	/*
	 * Perform 8 End Of Interrupt Command to make sure AIC
	 * will not Lock out nIRQ
	 */
	for (i = 0; i < 8; i++)
		irq_reg_writel(0, gc->reg_base + AT91_AIC_EOICR);

	/*
	 * Spurious Interrupt ID in Spurious Vector Register.
	 * When there is no current interrupt, the IRQ Vector Register
	 * reads the value stored in AIC_SPU
	 */
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC_SPU);

	/* No debugging in AIC: Debug (Protect) Control Register */
	irq_reg_writel(0, gc->reg_base + AT91_AIC_DCR);

	/* Disable and clear all interrupts initially */
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC_IDCR);
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC_ICCR);

	for (i = 0; i < 32; i++)
		irq_reg_writel(i, gc->reg_base + AT91_AIC_SVR(i));

	aic_mux_hw_init(domain);
}

static void __init aic5_hw_init(struct irq_domain *domain)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(domain, 0);
	int i;

	/*
	 * Perform 8 End Of Interrupt Command to make sure AIC
	 * will not Lock out nIRQ
	 */
	for (i = 0; i < 8; i++)
		irq_reg_writel(0, gc->reg_base + AT91_AIC5_EOICR);

	/*
	 * Spurious Interrupt ID in Spurious Vector Register.
	 * When there is no current interrupt, the IRQ Vector Register
	 * reads the value stored in AIC_SPU
	 */
	irq_reg_writel(0xffffffff, gc->reg_base + AT91_AIC5_SPU);

	/* No debugging in AIC: Debug (Protect) Control Register */
	irq_reg_writel(0, gc->reg_base + AT91_AIC5_DCR);

	/* Disable and clear all interrupts initially */
	for (i = 0; i < domain->revmap_size; i++) {
		irq_reg_writel(i, gc->reg_base + AT91_AIC5_SSR);
		irq_reg_writel(i, gc->reg_base + AT91_AIC5_SVR);
		irq_reg_writel(1, gc->reg_base + AT91_AIC5_IDCR);
		irq_reg_writel(1, gc->reg_base + AT91_AIC5_ICCR);
	}

	aic_mux_hw_init(domain);
}

static int at91_aic_common_irq_domain_xlate(struct irq_domain *d,
					    struct device_node *ctrlr,
					    const u32 *intspec,
					    unsigned int intsize,
					    irq_hw_number_t *out_hwirq,
					    unsigned int *out_type)
{
	if (WARN_ON(intsize < 3))
		return -EINVAL;

	if (WARN_ON((intspec[2] < AT91_AIC_IRQ_MIN_PRIORITY) ||
		    (intspec[2] > AT91_AIC_IRQ_MAX_PRIORITY)))
		return -EINVAL;

	*out_hwirq = intspec[0];
	*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static int aic_irq_domain_xlate(struct irq_domain *d,
				struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq,
				unsigned int *out_type)
{
	struct irq_domain_chip_generic *dgc = d->gc;
	struct irq_chip_generic *gc;
	unsigned long smr;
	int idx;
	int ret;

	if (!dgc)
		return -EINVAL;

	ret = at91_aic_common_irq_domain_xlate(d, ctrlr, intspec, intsize,
					       out_hwirq, out_type);
	if (ret)
		return ret;

	idx = intspec[0] / dgc->irqs_per_chip;
	if (idx >= dgc->num_chips)
		return -EINVAL;

	gc = dgc->gc[idx];

	irq_gc_lock(gc);
	smr = irq_reg_readl(gc->reg_base + AT91_AIC5_SMR) & ~AT91_AIC_PRIOR;
	irq_reg_writel(intspec[2] | smr, gc->reg_base + AT91_AIC5_SMR);
	irq_gc_unlock(gc);

	return 0;
}

static const struct irq_domain_ops aic_irq_ops = {
	.map	= irq_map_generic_chip,
	.xlate	= aic_irq_domain_xlate,
};

static int aic5_irq_domain_xlate(struct irq_domain *d,
				 struct device_node *ctrlr,
				 const u32 *intspec, unsigned int intsize,
				 irq_hw_number_t *out_hwirq,
				 unsigned int *out_type)
{
	struct irq_domain_chip_generic *dgc = d->gc;
	struct irq_chip_generic *gc;
	unsigned long smr;
	int ret;

	if (!dgc)
		return -EINVAL;

	ret = at91_aic_common_irq_domain_xlate(d, ctrlr, intspec, intsize,
					       out_hwirq, out_type);
	if (ret)
		return ret;

	gc = dgc->gc[0];

	irq_gc_lock(gc);
	irq_reg_writel(*out_hwirq, gc->reg_base + AT91_AIC5_SSR);
	smr = irq_reg_readl(gc->reg_base + AT91_AIC5_SMR) & ~AT91_AIC_PRIOR;
	irq_reg_writel(intspec[2] | smr, gc->reg_base + AT91_AIC5_SMR);
	irq_gc_unlock(gc);

	return 0;
}

static const struct irq_domain_ops aic5_irq_ops = {
	.map	= irq_map_generic_chip,
	.xlate	= aic5_irq_domain_xlate,
};

static struct aic_mux_irq *aic_mux_irq_of_init(struct device_node *node)
{
	struct aic_mux_irq *irq;
	struct of_phandle_args args;
	struct resource res;
	int ret;

	ret = of_parse_phandle_with_fixed_args(node, "atmel,aic-mux-irq-reg",
					       3, 0, &args);
	if (ret) {
		pr_warn("AIC: failed to retrieve atmel,aic-mux-irq-reg property\n");
		return ERR_PTR(ret);
	}

	ret = of_address_to_resource(args.np, args.args[0], &res);
	if (ret) {
		pr_warn("AIC: failed to retrieve muxed irq line iomem info\n");
		return ERR_PTR(ret);
	}

	if (resource_size(&res) < args.args[1]) {
		pr_warn("AIC: wrong disable register offset\n");
		return ERR_PTR(-EINVAL);
	}

	irq = kzalloc(sizeof(*irq), GFP_KERNEL);
	if (!irq)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&irq->node);

	irq->base = ioremap(res.start, resource_size(&res));
	if (!irq->base) {
		kfree(irq);
		return ERR_PTR(-ENOMEM);
	}
	irq->offset = args.args[1];
	irq->mask = args.args[2];

	return irq;
}

static struct aic_mux_irq *aic_mux_1reg_irq_of_init(struct device_node *node)
{
	struct aic_mux_irq *irq;

	irq = aic_mux_irq_of_init(node);
	if (!IS_ERR(irq))
		irq->type = AIC_MUX_1REG_IRQ;

	return irq;
}

static struct aic_mux_irq *aic_mux_3reg_irq_of_init(struct device_node *node)
{
	struct aic_mux_irq *irq;

	irq = aic_mux_irq_of_init(node);
	if (!IS_ERR(irq))
		irq->type = AIC_MUX_3REG_IRQ;

	return irq;
}

static const struct of_device_id aic_mux_irq_of_match[] __initconst = {
	{
		.compatible = "atmel,aic-mux-1reg-irq",
		.data = aic_mux_1reg_irq_of_init,
	},
	{
		.compatible = "atmel,aic-mux-3reg-irq",
		.data = aic_mux_3reg_irq_of_init,
	},
	{ /*sentinel*/ }
};

static const struct of_device_id aic_mux_of_match[] __initconst = {
	{ .compatible = "atmel,aic-mux" },
	{ /*sentinel*/ }
};

static void __init aic_ext_irq_of_init(struct irq_domain *domain)
{
	struct device_node *node = domain->of_node;
	struct irq_chip_generic *gc;
	struct aic_chip_data *aic;
	struct property *prop;
	const __be32 *p;
	u32 hwirq;

	gc = irq_get_domain_generic_chip(aic_domain, 0);

	aic = gc->private;
	aic->ext_irqs |= 1;

	of_property_for_each_u32(node, "atmel,external-irqs", prop, p, hwirq) {
		gc = irq_get_domain_generic_chip(aic_domain, hwirq);
		if (!gc) {
			pr_warn("AIC: external irq %d >= %d skip it\n",
				hwirq, domain->revmap_size);
			continue;
		}

		aic = gc->private;
		aic->ext_irqs |= (1 << (hwirq % 32));
	}
}

static void __init aic_mux_of_init(struct irq_domain *domain)
{
	struct device_node *node = domain->of_node;
	struct device_node *irq_node;
	struct device_node *mux_node;
	const struct of_device_id *match;
	struct aic_mux_irq *irq;
	struct aic_mux_irq *(*mux_of_init)(struct device_node *);
	struct irq_chip_generic *gc;
	struct aic_chip_data *aic;
	struct list_head *mux_list;
	u32 hwirq;

	for_each_child_of_node(node, mux_node) {
		if (!of_match_node(aic_mux_of_match, mux_node))
			continue;

		if (of_property_read_u32(mux_node, "reg", &hwirq)) {
			pr_warn("AIC: missing reg property in mux definition\n");
			continue;
		}

		gc = irq_get_domain_generic_chip(aic_domain, hwirq);
		if (!gc) {
			pr_warn("AIC: irq %d >= %d skip it\n",
				hwirq, domain->revmap_size);
			continue;
		}

		aic = gc->private;
		mux_list = &aic->mux[hwirq % 32];

		for_each_child_of_node(mux_node, irq_node) {
			match = of_match_node(aic_mux_irq_of_match, irq_node);
			if (!match)
				continue;

			mux_of_init = match->data;

			irq = mux_of_init(irq_node);
			if (IS_ERR(irq))
				continue;

			list_add_tail(&irq->node, mux_list);
		}
	}
}

static int __init aic_common_of_init(struct device_node *node,
				     const struct irq_domain_ops *ops,
				     const char *name, int maxirq)
{
	struct irq_chip_generic *gc;
	struct aic_chip_data *aic;
	void __iomem *reg_base;
	int nirqs = maxirq;
	int nchips;
	int ret;
	int i;
	int j;
	u32 tmp;

	if (aic_domain)
		return -EEXIST;

	if (of_get_property(node, "atmel,aic-irq-mapping", &tmp))
		nirqs = tmp * BITS_PER_BYTE;

	nchips = DIV_ROUND_UP(nirqs, 32);

	reg_base = of_iomap(node, 0);
	if (!reg_base)
		return -ENOMEM;

	aic = kzalloc(nchips * sizeof(*aic), GFP_KERNEL);
	if (!aic) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	aic_domain = irq_domain_add_linear(node, nirqs, ops, aic);
	if (!aic_domain) {
		ret = -ENOMEM;
		goto err_free_aic;
	}

	ret = irq_alloc_domain_generic_chips(aic_domain, 32, 1, name,
					     handle_level_irq, 0, 0,
					     IRQCHIP_SKIP_SET_WAKE);
	if (ret)
		goto err_domain_remove;

	for (i = 0; i < nchips; i++) {
		gc = irq_get_domain_generic_chip(aic_domain, i * 32);

		gc->reg_base = reg_base;

		if (!of_property_read_u32_index(node, "atmel,irq-mapping",
						i, &tmp)) {
			gc->unused = ~tmp;
			gc->wake_enabled = tmp;
		} else {
			gc->unused = 0;
			gc->wake_enabled = ~0;
		}

		gc->chip_types[0].type = IRQ_TYPE_SENSE_MASK;
		gc->chip_types[0].handler = handle_fasteoi_irq;
		gc->chip_types[0].chip.irq_eoi = irq_gc_eoi;
		gc->chip_types[0].chip.irq_set_wake = irq_gc_set_wake;
		gc->chip_types[0].chip.irq_shutdown = aic_shutdown;

		for (j = 0; j < 32; j++)
			INIT_LIST_HEAD(&aic[i].mux[j]);

		gc->private = &aic[i];
	}

	aic_mux_of_init(aic_domain);
	aic_ext_irq_of_init(aic_domain);

	return 0;

err_domain_remove:
	irq_domain_remove(aic_domain);

err_free_aic:
	kfree(aic);

err_iounmap:
	iounmap(reg_base);

	return ret;
}

static int __init aic_of_init(struct device_node *node,
			      struct device_node *parent)
{
	struct irq_chip_generic *gc;
	int ret;

	ret = aic_common_of_init(node, &aic_irq_ops, "atmel-aic",
				 NR_AIC_IRQS);
	if (ret)
		return ret;

	gc = irq_get_domain_generic_chip(aic_domain, 0);

	gc->chip_types[0].regs.eoi = AT91_AIC_EOICR;
	gc->chip_types[0].regs.enable = AT91_AIC_IECR;
	gc->chip_types[0].regs.disable = AT91_AIC_IDCR;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_disable_reg;
	gc->chip_types[0].chip.irq_unmask = irq_gc_unmask_enable_reg;
	gc->chip_types[0].chip.irq_retrigger = aic_retrigger;
	gc->chip_types[0].chip.irq_set_type = aic_set_type;
	gc->chip_types[0].chip.irq_suspend = aic_suspend;
	gc->chip_types[0].chip.irq_resume = aic_resume;
	gc->chip_types[0].chip.irq_pm_shutdown = aic_pm_shutdown;

	aic_hw_init(aic_domain);
	set_handle_irq(aic_handle);

	return 0;
}
IRQCHIP_DECLARE(at91_aic, "atmel,at91rm9200-aic", aic_of_init);

static int __init aic5_of_init(struct device_node *node,
			       struct device_node *parent)
{
	struct irq_chip_generic *gc;
	int ret;
	int i;
	int nchips;

	ret = aic_common_of_init(node, &aic5_irq_ops, "atmel-aic5",
				 NR_AIC5_IRQS);
	if (ret)
		return ret;

	nchips = aic_domain->revmap_size / 32;
	for (i = 0; i < nchips; i++) {
		gc = irq_get_domain_generic_chip(aic_domain, i * 32);

		gc->chip_types[0].regs.eoi = AT91_AIC5_EOICR;
		gc->chip_types[0].chip.irq_mask = aic5_mask;
		gc->chip_types[0].chip.irq_unmask = aic5_unmask;
		gc->chip_types[0].chip.irq_retrigger = aic5_retrigger;
		gc->chip_types[0].chip.irq_set_type = aic5_set_type;
		gc->chip_types[0].chip.irq_suspend = aic5_suspend;
		gc->chip_types[0].chip.irq_resume = aic5_resume;
		gc->chip_types[0].chip.irq_pm_shutdown = aic5_pm_shutdown;
	}

	aic5_hw_init(aic_domain);
	set_handle_irq(aic5_handle);

	return 0;
}
IRQCHIP_DECLARE(at91_aic5, "atmel,sama5d3-aic", aic5_of_init);
