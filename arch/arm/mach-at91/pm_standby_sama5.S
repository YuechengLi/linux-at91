/*
 * Copyright (C) 2014 Atmel,
 *
 * This program is free software,you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <linux/clk/at91_pmc.h>
#include <mach/at91_ramc.h>

pmc		.req	r0
ddrc		.req	r1
tmp		.req	r4
saved_lpr	.req	r5

/*
 * void sama5_do_suspend(void *pmc, void *ddrc)
 *
 * This function code puts the DDR device into self-refresh mode,
 * disables the processor's clock, puts the CPU to the WFI state,
 * and waits for interrupt to wake up.
 */
ENTRY(sama5_do_suspend)
	stmfd	sp!, {r4 - r12, lr}

	/* Active self-fresh mode for DDR device */
	ldr	tmp, [ddrc, #AT91_DDRSDRC_LPR]
	mov	saved_lpr, tmp

	bic	tmp, #AT91_DDRSDRC_LPCB
	orr	tmp, #AT91_DDRSDRC_LPCB_SELF_REFRESH
	str	tmp, [ddrc, #AT91_DDRSDRC_LPR]

	/*
	 * Execute an ISB instruction to ensure that all of the
	 * CP15 register changes have been committed.
	 */
	isb

	/*
	 * Execute a barrier instruction to ensure that all cache,
	 * TLB and branch predictor maintenance operations issued
	 * by any CPU have completed.
	 */
	dsb
	dmb

	/* Disable the Processor Clock */
	mov	tmp, #AT91_PMC_PCK
	str	tmp, [pmc, #AT91_PMC_SCDR]

	/* Execute a WFI instruction */
	wfi	@ Wait For Interrupt

	/*
	 * CPU can specualatively prefetch the instructions
	 * so add NOPs after WFI. Sixteen NOPs as Cortex-A5 pipeline.
	 */
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop

	/* Restore the LPR to prepare for the self-fresh mode exit */
	str	saved_lpr, [ddrc, #AT91_DDRSDRC_LPR]

	ldmfd	sp!, {r4 - r12, pc}
ENDPROC(sama5_do_suspend)

/*
 * void sama5_ddr_standby(void)
 *
 * This function code disables the L1/L2 caches,
 * puts the DDR device into the self-fresh mode,
 * disables the processor's clock and puts the processor into the WFI state.
 */
ENTRY(sama5_ddr_standby)
	stmfd	sp!, {r4 - r12, lr}

	bl	sama5_disable_cache

	ldr	tmp, at91_ramc0_base_addr	@ get addr
	ldr	r1, [tmp]			@ get value

	ldr	tmp, at91_pmc_base_addr		@get addr
	ldr	r0, [tmp]			@ get value

	bl	sama5_do_suspend

	bl	sama5_enable_cache

	ldmfd	sp!, {r4 - r12, pc}
ENDPROC(sama5_ddr_standby)

at91_pmc_base_addr:
	.word	at91_pmc_base

at91_ramc0_base_addr:
	.word	at91_ramc_base
