/*
 * Copyright (C) 2014 Atmel,
 *
 * This program is free software,you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <asm/hardware/cache-l2x0.h>

/*
 * void sama5_disable_cache(void)
 *
 * This function code flushes the L1 data cache, disables the L1 data cache,
 * cleans and invalidates the L2 cache, then disable the L2 cache.
 */
ENTRY(sama5_disable_cache)
	stmfd	sp!, {r4 - r12, lr}

	/*
	 * Flush all data from the L1 data cache before disabling
	 * SCTLR.C bit.
	 */
	bl	v7_flush_dcache_all

	/*
	 * Clear the SCTLR.C bit to prevent further data cache
	 * allocation. Clearing SCTLR.C would make all the data accesses
	 * strongly ordered and would not hit the cache.
	 */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 2)	@ Disable the C bit
	mcr	p15, 0, r0, c1, c0, 0
	isb

	/*
	 * Invalidate L1 data cache. Even though only invalidate is
	 * necessary exported flush API is used here. Doing clean
	 * on already clean cache would be almost NOP.
	 */
	bl	v7_flush_dcache_all

	/*
	 * Clean and invalidate the L2 cache.
	 * Common cache-l2x0.c functions can't be used here since it
	 * uses spinlocks. We are out of coherency here with data cache
	 * disabled. The spinlock implementation uses exclusive load/store
	 * instruction which can fail without data cache being enabled.
	 * Because of this, CPU can lead to deadlock.
	 */
	ldr	r1, at91_l2cc_base_addr
	ldr	r2, [r1]
	cmp	r2, #0
	beq	skip_l2disable
	mov	r0, #0xff
	str	r0, [r2, #L2X0_CLEAN_INV_WAY]
wait:
	ldr	r0, [r2, #L2X0_CLEAN_INV_WAY]
	mov	r1, #0xff
	ands	r0, r0, r1
	bne	wait

	mov	r0, #0
	str	r0, [r2, #L2X0_CTRL]

l2x_sync:
	ldr	r0, [r2, #L2X0_CACHE_SYNC]
	bic	r0, r0, #0x1
	str	r0, [r2, #L2X0_CACHE_SYNC]
sync:
	ldr	r0, [r2, #L2X0_CACHE_SYNC]
	ands	r0, r0, #0x1
	bne	sync

skip_l2disable:
	ldmfd	sp!, {r4 - r12, pc}
ENDPROC(sama5_disable_cache)

/*
 * void sama5_enable_cache(void)
 *
 * This function code enables the L1 data cache and the L2 cache.
 */
ENTRY(sama5_enable_cache)
	stmfd	sp!, {r4 - r12, lr}

	/* Enable the L2 cache */
	ldr	r1, at91_l2cc_base_addr
	ldr	r2, [r1]
	cmp	r2, #0
	beq	skip_l2en
	ldr	r0, [r2, #L2X0_CTRL]
	ands	r0, r0, #L2X0_CTRL_EN
	bne	skip_l2en	@ Skip if already enabled
	mov	r0, #L2X0_CTRL_EN
	str	r0, [r2, #L2X0_CTRL]
skip_l2en:

	/* Enable the L1 data cache */
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #(1 << 2)		@ Check C bit enabled?
	orreq	r0, r0, #(1 << 2)	@ Enable the C bit
	mcreq	p15, 0, r0, c1, c0, 0
	isb

	ldmfd	sp!, {r4 - r12, pc}
ENDPROC(sama5_enable_cache)

at91_l2cc_base_addr:
	.word	at91_l2cc_base
