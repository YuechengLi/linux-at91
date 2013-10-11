/*
 * drivers/clk/at91/clk-system.c
 *
 *  Copyright (C) 2013 Boris BREZILLON <b.brezillon@overkiz.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clk/at91_pmc.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>

#include "pmc.h"

#define SYSTEM_MAX_ID		31

#define to_clk_system(hw) container_of(hw, struct clk_system, hw)
struct clk_system {
	struct clk_hw hw;
	struct at91_pmc *pmc;
	u8 id;
};

static int clk_system_enable(struct clk_hw *hw)
{
	struct clk_system *sys = to_clk_system(hw);
	struct at91_pmc *pmc = sys->pmc;

	pmc_write(pmc, AT91_PMC_SCER, 1 << sys->id);
	return 0;
}

static void clk_system_disable(struct clk_hw *hw)
{
	struct clk_system *sys = to_clk_system(hw);
	struct at91_pmc *pmc = sys->pmc;

	pmc_write(pmc, AT91_PMC_SCDR, 1 << sys->id);
}

static int clk_system_is_enabled(struct clk_hw *hw)
{
	struct clk_system *sys = to_clk_system(hw);
	struct at91_pmc *pmc = sys->pmc;

	return !!(pmc_read(pmc, AT91_PMC_SCSR) & (1 << sys->id));
}

static const struct clk_ops system_ops = {
	.enable = clk_system_enable,
	.disable = clk_system_disable,
	.is_enabled = clk_system_is_enabled,
};

static struct clk * __init
at91_clk_register_system(struct at91_pmc *pmc, const char *name,
			 const char *parent_name, u8 id)
{
	struct clk_system *sys;
	struct clk *clk = NULL;
	struct clk_init_data init;

	if (!parent_name || id > SYSTEM_MAX_ID)
		return ERR_PTR(-EINVAL);

	sys = kzalloc(sizeof(*sys), GFP_KERNEL);
	if (!sys)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &system_ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	/*
	 * CLK_IGNORE_UNUSED is used to avoid ddrck switch off.
	 * TODO : we should implement a driver supporting at91 ddr controller
	 * (see drivers/memory) which would request and enable the ddrck clock.
	 * When this is done we will be able to remove CLK_IGNORE_UNUSED flag.
	 */
	init.flags = CLK_IGNORE_UNUSED;

	sys->id = id;
	sys->hw.init = &init;
	sys->pmc = pmc;

	clk = clk_register(NULL, &sys->hw);
	if (IS_ERR(clk))
		kfree(sys);

	return clk;
}

struct clk_system_data {
	struct clk **clks;
	u8 *ids;
	unsigned int clk_num;
};

static struct clk * __init
of_clk_src_system_get(struct of_phandle_args *clkspec, void *data)
{
	struct clk_system_data *clk_data = data;
	unsigned int id = clkspec->args[0];
	int i;

	if (id > SYSTEM_MAX_ID)
		goto err;

	for (i = 0; i < clk_data->clk_num; i++) {
		if (clk_data->ids[i] == id)
			return clk_data->clks[i];
	}

err:
	pr_err("%s: invalid clock id %d\n", __func__, id);
	return ERR_PTR(-EINVAL);
}

static void __init
of_at91_clk_sys_setup(struct device_node *np, struct at91_pmc *pmc)
{
	int i;
	int num;
	u32 id;
	struct clk *clk;
	u8 *ids;
	struct clk **clks;
	struct clk_system_data *clktab;
	const char *name;
	struct device_node *sysclknp;
	const char *parent_name;

	num = of_get_child_count(np);
	if (num > (SYSTEM_MAX_ID + 1))
		return;

	clktab = kzalloc(sizeof(*clktab), GFP_KERNEL);
	if (!clktab)
		return;

	ids = kzalloc(num * sizeof(*ids), GFP_KERNEL);
	if (!ids)
		goto out_free_clktab;

	clks = kzalloc(num * sizeof(*clks), GFP_KERNEL);
	if (!clks)
		goto out_free_ids;

	i = 0;
	for_each_child_of_node(np, sysclknp) {
		name = sysclknp->name;

		if (of_property_read_u32(sysclknp, "atmel,clk-id", &id))
			goto out_free_clks;

		parent_name = of_clk_get_parent_name(sysclknp, 0);

		clk = at91_clk_register_system(pmc, name, parent_name, id);
		if (IS_ERR(clk))
			goto out_free_clks;

		clks[i] = clk;
		ids[i] = id;

		i++;
	}

	clktab->clk_num = num;
	clktab->clks = clks;
	clktab->ids = ids;
	of_clk_add_provider(np, of_clk_src_system_get, clktab);
	return;

out_free_clks:
	kfree(clks);
out_free_ids:
	kfree(ids);
out_free_clktab:
	kfree(clktab);
}

void __init of_at91rm9200_clk_sys_setup(struct device_node *np,
					struct at91_pmc *pmc)
{
	of_at91_clk_sys_setup(np, pmc);
}
