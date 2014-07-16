/*
 *  Setup code for SAMA5 Evaluation Kits with Device Tree support
 *
 *  Copyright (C) 2013 Atmel,
 *                2013 Ludovic Desroches <ludovic.desroches@atmel.com>
 *
 * Licensed under GPLv2 or later.
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/micrel_phy.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>

#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/hardware/cache-l2x0.h>

#include "generic.h"

void __iomem *at91_l2cc_base;
EXPORT_SYMBOL_GPL(at91_l2cc_base);

#ifdef CONFIG_CACHE_L2X0
static void __init at91_init_l2cache(void)
{
	struct device_node *np;
	u32 reg;

	np = of_find_compatible_node(NULL, NULL, "arm,pl310-cache");
	if (!np)
		return;

	at91_l2cc_base = of_iomap(np, 0);
	if (!at91_l2cc_base)
		panic("unable to map l2cc cpu registers\n");

	of_node_put(np);

	/* Disable cache if it hasn't been done yet */
	if (readl_relaxed(at91_l2cc_base + L2X0_CTRL) & L2X0_CTRL_EN)
		writel_relaxed(~L2X0_CTRL_EN, at91_l2cc_base + L2X0_CTRL);

	/* Prefetch Control */
	reg = readl_relaxed(at91_l2cc_base + L310_PREFETCH_CTRL);
	reg &= ~L310_PREFETCH_CTRL_OFFSET_MASK;
	reg |= 0x01;
	reg |= L310_PREFETCH_CTRL_DBL_LINEFILL_INCR;
	reg |= L310_PREFETCH_CTRL_PREFETCH_DROP;
	reg |= L310_PREFETCH_CTRL_DATA_PREFETCH;
	reg |= L310_PREFETCH_CTRL_INSTR_PREFETCH;
	reg |= L310_PREFETCH_CTRL_DBL_LINEFILL;
	writel_relaxed(reg, at91_l2cc_base + L310_PREFETCH_CTRL);

	/* Power Control */
	reg = readl_relaxed(at91_l2cc_base + L310_POWER_CTRL);
	reg |= L310_STNDBY_MODE_EN;
	reg |= L310_DYNAMIC_CLK_GATING_EN;
	writel_relaxed(reg, at91_l2cc_base + L310_POWER_CTRL);

	/* Disable interrupts */
	writel_relaxed(0x00, at91_l2cc_base + L2X0_INTR_MASK);
	writel_relaxed(0x01ff, at91_l2cc_base + L2X0_INTR_CLEAR);
	l2x0_of_init(0, ~0UL);
}
#else
static inline void at91_init_l2cache(void) {}
#endif

static void __init sama5_dt_device_init(void)
{
	at91_init_l2cache();

	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
}

static const char *sama5_dt_board_compat[] __initconst = {
	"atmel,sama5",
	NULL
};

DT_MACHINE_START(sama5_dt, "Atmel SAMA5 (Device Tree)")
	/* Maintainer: Atmel */
	.map_io		= at91_map_io,
	.init_early	= at91_dt_initialize,
	.init_machine	= sama5_dt_device_init,
	.dt_compat	= sama5_dt_board_compat,
MACHINE_END

static const char *sama5_alt_dt_board_compat[] __initconst = {
	"atmel,sama5d4",
	NULL
};

DT_MACHINE_START(sama5_alt_dt, "Atmel SAMA5 (Device Tree)")
	/* Maintainer: Atmel */
	.map_io		= at91_alt_map_io,
	.init_early	= at91_dt_initialize,
	.init_machine	= sama5_dt_device_init,
	.dt_compat	= sama5_alt_dt_board_compat,
	.l2c_aux_mask	= ~0UL,
MACHINE_END
