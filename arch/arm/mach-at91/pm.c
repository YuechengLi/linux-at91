/*
 * arch/arm/mach-at91/pm.c
 * AT91 Power Management
 *
 * Copyright (C) 2005 David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/gpio.h>
#include <linux/suspend.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/clk/at91_pmc.h>
#include <linux/genalloc.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include <asm/irq.h>
#include <linux/atomic.h>
#include <asm/mach/time.h>
#include <asm/mach/irq.h>
#include <asm/fncpy.h>

#include <mach/cpu.h>
#include <mach/hardware.h>

#include "generic.h"
#include "pm.h"

static void (*at91_suspend_sram_fn)(void __iomem *pmc, void __iomem *ramc0,
					 void __iomem *ramc1, int memctrl);

static void (*at91_pm_standby)(void);

static int at91_pm_valid_state(suspend_state_t state)
{
	switch (state) {
		case PM_SUSPEND_ON:
		case PM_SUSPEND_STANDBY:
		case PM_SUSPEND_MEM:
			return 1;

		default:
			return 0;
	}
}


static suspend_state_t target_state;

/*
 * Called after processes are frozen, but before we shutdown devices.
 */
static int at91_pm_begin(suspend_state_t state)
{
	target_state = state;
	return 0;
}

/*
 * Verify that all the clocks are correct before entering
 * slow-clock mode.
 */
static int at91_pm_verify_clocks(void)
{
	unsigned long scsr;
	int i;

	scsr = at91_pmc_read(AT91_PMC_SCSR);

	/* USB must not be using PLLB */
	if (cpu_is_at91rm9200()) {
		if ((scsr & (AT91RM9200_PMC_UHP | AT91RM9200_PMC_UDP)) != 0) {
			pr_err("AT91: PM - Suspend-to-RAM with USB still active\n");
			return 0;
		}
	} else if (cpu_is_at91sam9260() || cpu_is_at91sam9261() || cpu_is_at91sam9263()
			|| cpu_is_at91sam9g20() || cpu_is_at91sam9g10()) {
		if ((scsr & (AT91SAM926x_PMC_UHP | AT91SAM926x_PMC_UDP)) != 0) {
			pr_err("AT91: PM - Suspend-to-RAM with USB still active\n");
			return 0;
		}
	}

	/* PCK0..PCK3 must be disabled, or configured to use clk32k */
	for (i = 0; i < 4; i++) {
		u32 css;

		if ((scsr & (AT91_PMC_PCK0 << i)) == 0)
			continue;

		css = at91_pmc_read(AT91_PMC_PCKR(i)) & AT91_PMC_CSS;
		if (css != AT91_PMC_CSS_SLOW) {
			pr_err("AT91: PM - Suspend-to-RAM with PCK%d src %d\n", i, css);
			return 0;
		}
	}

	/* Drivers should have previously suspended USB PLL */
	if (at91_pmc_read(AT91_CKGR_UCKR) & AT91_PMC_UPLLEN) {
		pr_err("AT91: PM - Suspend-to-RAM with USB PLL running\n");
		return 0;
	}

	/* Drivers should have previously suspended PLL B */
	if (at91_pmc_read(AT91_PMC_SR) & AT91_PMC_LOCKB) {
		pr_err("AT91: PM - Suspend-to-RAM with PLL B running\n");
		return 0;
	}

	return 1;
}

/*
 * Call this from platform driver suspend() to see how deeply to suspend.
 * For example, some controllers (like OHCI) need one of the PLL clocks
 * in order to act as a wakeup source, and those are not available when
 * going into slow clock mode.
 *
 * REVISIT: generalize as clk_will_be_available(clk)?  Other platforms have
 * the very same problem (but not using at91 main_clk), and it'd be better
 * to add one generic API rather than lots of platform-specific ones.
 */
int at91_suspend_entering_slow_clock(void)
{
	return (target_state == PM_SUSPEND_MEM);
}
EXPORT_SYMBOL(at91_suspend_entering_slow_clock);

static int at91_pm_enter(suspend_state_t state)
{
	int memctrl = AT91_MEMCTRL_SDRAMC;

	at91_pinctrl_gpio_suspend();

	switch (state) {
		/*
		 * Suspend-to-RAM is like STANDBY plus slow clock mode, so
		 * drivers must suspend more deeply:  only the master clock
		 * controller may be using the main oscillator.
		 */
		case PM_SUSPEND_MEM:
			/*
			 * Ensure that clocks are in a valid state.
			 */
			if (!at91_pm_verify_clocks())
				goto error;

			/*
			 * Jump to the internal SRAM, the master clock
			 * swithes to slow clock, turn off the main oscillator,
			 * cpu enters into the WFI state, wait for wake up.
			 */
			if (cpu_is_at91rm9200())
				memctrl = AT91_MEMCTRL_MC;
			else if (cpu_is_at91sam9g45()
				|| cpu_is_at91sam9x5()
				|| cpu_is_at91sam9n12()
				|| cpu_is_sama5d3()
				|| cpu_is_sama5d4())
				memctrl = AT91_MEMCTRL_DDRSDR;

			at91_suspend_sram_fn(at91_pmc_base, at91_ramc_base[0],
						at91_ramc_base[1], memctrl);

			break;

		/*
		 * STANDBY mode has *all* drivers suspended; ignores irqs not
		 * marked as 'wakeup' event sources; and reduces DRAM power.
		 * But otherwise it's identical to PM_SUSPEND_ON:  cpu idle, and
		 * nothing fancy done with main or cpu clocks.
		 */
		case PM_SUSPEND_STANDBY:
			/*
			 * NOTE: the Wait-for-Interrupt instruction needs to be
			 * in icache so no SDRAM accesses are needed until the
			 * wakeup IRQ occurs and self-refresh is terminated.
			 * For ARM 926 based chips, this requirement is weaker
			 * as at91sam9 can access a RAM in self-refresh mode.
			 */
			if (at91_pm_standby)
				at91_pm_standby();
			break;

		case PM_SUSPEND_ON:
			cpu_do_idle();
			break;

		default:
			pr_debug("AT91: PM - bogus suspend state %d\n", state);
			goto error;
	}

error:
	target_state = PM_SUSPEND_ON;

	at91_pinctrl_gpio_resume();
	return 0;
}

/*
 * Called right prior to thawing processes.
 */
static void at91_pm_end(void)
{
	target_state = PM_SUSPEND_ON;
}


static const struct platform_suspend_ops at91_pm_ops = {
	.valid	= at91_pm_valid_state,
	.begin	= at91_pm_begin,
	.enter	= at91_pm_enter,
	.end	= at91_pm_end,
};

static struct platform_device at91_cpuidle_device = {
	.name = "cpuidle-at91",
};

void at91_pm_set_standby(void (*at91_standby)(void))
{
	if (at91_standby) {
		at91_cpuidle_device.dev.platform_data = at91_standby;
		at91_pm_standby = at91_standby;
	}
}

#define	SUSPEND_SRAM_SIZE	0x10000

extern void at91_pm_suspend_in_sram(void __iomem *pmc, void __iomem *ramc0,
					void __iomem *ramc1, int memctrl);
extern u32 at91_pm_suspend_in_sram_sz;

static void __iomem *suspend_sram_base;

static int __init at91_suspend_init(void)
{
	phys_addr_t sram_pbase;
	struct device_node *node;
	struct platform_device *pdev;
	struct gen_pool *sram_pool;
	unsigned long sram_base;
	int ret = 0;

	node = of_find_compatible_node(NULL, NULL, "mmio-sram");
	if (!node) {
		pr_warn("%s: failed to find sram node!\n", __func__);
		return -ENODEV;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		pr_warn("%s: failed to find sram device!\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	sram_pool = dev_get_gen_pool(&pdev->dev);
	if (!sram_pool) {
		pr_warn("%s: sram pool unavailable!\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	sram_base = gen_pool_alloc(sram_pool, SUSPEND_SRAM_SIZE);
	if (!sram_base) {
		pr_warn("%s: unable to alloc sram!\n", __func__);
		ret = -ENOMEM;
		goto put_node;
	}

	sram_pbase = gen_pool_virt_to_phys(sram_pool, sram_base);

	suspend_sram_base = __arm_ioremap_exec(sram_pbase,
						SUSPEND_SRAM_SIZE, false);

	at91_suspend_sram_fn = fncpy(suspend_sram_base,
					&at91_pm_suspend_in_sram,
					at91_pm_suspend_in_sram_sz);

	suspend_set_ops(&at91_pm_ops);

put_node:
	of_node_put(node);

	return ret;
}

static int __init at91_pm_init(void)
{
	/* AT91RM9200 SDRAM low-power mode cannot be used with self-refresh. */
	if (cpu_is_at91rm9200())
		at91_ramc_write(0, AT91RM9200_SDRAMC_LPR, 0);
	
	if (at91_cpuidle_device.dev.platform_data)
		platform_device_register(&at91_cpuidle_device);

	at91_suspend_init();

	return 0;
}
arch_initcall(at91_pm_init);
