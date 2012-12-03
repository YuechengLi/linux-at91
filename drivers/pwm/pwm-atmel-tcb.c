/*
 * Copyright (C) Overkiz SAS 2012
 *
 * Author: Boris BREZILLON <b.brezillon@overkiz.com>
 * License terms: GNU General Public License (GPL) version 2
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/atmel_tc.h>
#include <linux/pwm.h>
#include <linux/of_device.h>
#include <linux/slab.h>

#define NPWM	6

/*
#define ATMEL_TC_EFFECT_NONE	0x0
#define ATMEL_TC_EFFECT_SET		0x1
#define ATMEL_TC_EFFECT_CLEAR	0x2
#define ATMEL_TC_EFFECT_TOGGLE	0x3
*/

//struct atmel_tcb_pwm_device {
//	u8 output; /* CPX_X | CPC_X | XEEVT_X | SWTRG_X */
//	u8 enabled : 1;
//	u8 clk : 4;
//	u16 config; /* CMR Lower 16 bits */
//	u32 period; /* CR */
//};

struct atmel_tcb_pwm_chip {
	struct pwm_chip chip;
	spinlock_t lock;
	struct atmel_tc *tc;
//	struct atmel_tcb_pwm pwms[NPWM];
};

static inline struct atmel_tcb_pwm_chip *to_atmel_tcb_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct atmel_tcb_pwm_chip, chip);
}

static int atmel_tcb_pwm_set_polarity (struct pwm_chip *chip, struct pwm_device *pwm,
					  enum pwm_polarity polarity)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = &(tcbpwmc->pwms[pwm->hwpwm]);
	struct atmel_tc *tc = tcbpwmc->tc;
	void __iomem *regs = tc->regs;
	u32 reg;

	spin_lock(&tcbpwmc->lock);
	reg = __raw_readl (regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));

	if (pwm->hwpwm % 2) {
		reg &= ~ATMEL_TC_BSWTRG;
		reg |= (polarity == PWM_POLARITY_NORMAL) ? ATMEL_TC_BSWTRG_SET : ATMEL_TC_BSWTRG_CLEAR;
	} else {
		reg &= ~ATMEL_TC_ASWTRG;
		reg |= (polarity == PWM_POLARITY_NORMAL) ? ATMEL_TC_ASWTRG_SET : ATMEL_TC_ASWTRG_CLEAR;
	}

	__raw_writel (reg, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	spin_unlock(&tcbpwmc->lock);

	return 0;
}

static int atmel_tcb_pwm_request (struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = tcbpwmc->pwms[pwm->hwpwm];
	struct atmel_tc *tc = tcbpwmc->tc;
	void __iomem *regs = tc->regs;
	u32 reg;

	spin_lock(&tcbpwmc->lock);
	reg = __raw_readl (regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	reg &= ~(ATMEL_TC_TCCLKS |
			ATMEL_TC_ACPA | ATMEL_TC_ACPC | ATMEL_TC_AEEVT | ATMEL_TC_ASWTRG |
			ATMEL_TC_BCPB | ATMEL_TC_BCPC | ATMEL_TC_BEEVT | ATMEL_TC_BSWTRG);
	reg |= ATMEL_TC_WAVE | ATMEL_TC_WAVESEL_UP_AUTO | ATMEL_TC_EEVT_XC0;
	spin_unlock(&tcbpwmc->lock);
	/* set normal polarity */
	atmel_tcb_pwm_set_polarity (chip, pwm, PWM_POLARITY_NORMAL);

	clk_enable (tc->clk[pwm->hwpwm / 2]);

	return 0;
}

static void atmel_tcb_pwm_free (struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = tcbpwmc->pwms[pwm->hwpwm];
	struct atmel_tc *tc = tcbpwmc->tc;

//	tcbpwm->output = 0;
//	tcbpwm->config = 0;
//	tcbpwm->period = 0;

	clk_disable (tc->clk[pwm->hwpwm / 2]);
}

static void atmel_tcb_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = &(tcbpwmc->pwms[pwm->hwpwm]);
	struct atmel_tc *tc = tcbpwmc->tc;
	void __iomem *regs = tc->regs;
	u32 reg;

//	tcbpwm->output = 0;
	spin_lock(&tcbpwmc->lock);
	reg = __raw_readl(regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	reg &= ~(0xFF << (pwm->hwpwm % 2) ? 24 : 16);
	if (pwm->hwpwm % 2) {
		reg &= ~(ATMEL_TC_BCPC | ATMEL_TC_BCPB);
//		reg |= ((reg & ATMEL_TC_BSWTRG) == ATMEL_TC_BSWTRG_SET) ? ATMEL_TC_BCPC_SET : ATMEL_TC_BCPC_CLEAR;
	} else {
		reg &= ~(ATMEL_TC_ACPC | ATMEL_TC_ACPA);
//		reg |= ((reg & ATMEL_TC_ASWTRG) == ATMEL_TC_ASWTRG_SET) ? ATMEL_TC_ACPC_SET : ATMEL_TC_ACPC_CLEAR;
	}
	__raw_writel (reg, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	__raw_writel (ATMEL_TC_SWTRG, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CCR));

	if (!(reg & (ATMEL_TC_ACPC | ATMEL_TC_BCPC)))
		__raw_writel(ATMEL_TC_CLKDIS, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CCR));
	spin_unlock(&tcbpwmc->lock);
}

static int atmel_tcb_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = &(tcbpwmc->pwms[pwm->hwpwm]);
	struct atmel_tc *tc = tcbpwmc->tc;
	void __iomem *regs = tc->regs;
	u32 reg;

	spin_lock(&tcbpwmc->lock);
	reg = __raw_readl(regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	if (pwm->hwpwm % 2) {
		reg &= ~ATMEL_TC_BCPC;
		reg |= ((reg & ATMEL_TC_BSWTRG) == ATMEL_TC_BSWTRG_SET) ? ATMEL_TC_BCPC_SET : ATMEL_TC_BCPC_CLEAR;
//		if (reg & ATMEL_TC_BCPB) {
//			reg &= ~ATMEL_TC_BCPB;
//			reg |= ((reg & ATMEL_TC_BSWTRG) == ATMEL_TC_BSWTRG_SET)? ATMEL_TC_BCPB_CLEAR : ATMEL_TC_BCPB_SET;
//		}
	} else {
		reg &= ~ATMEL_TC_ACPC;
		reg |= ((reg & ATMEL_TC_ASWTRG) == ATMEL_TC_ASWTRG_SET) ? ATMEL_TC_ACPC_SET : ATMEL_TC_ACPC_CLEAR;
//		if (reg & ATMEL_TC_ACPA) {
//			reg &= ~ATMEL_TC_ACPA;
//			reg |= ((reg & ATMEL_TC_ASWTRG) == ATMEL_TC_ASWTRG_SET)? ATMEL_TC_ACPA_CLEAR : ATMEL_TC_ACPA_SET;
//		}
	}
	__raw_writel (reg, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	reg = __raw_readl (regs + ATMEL_TC_REG(pwm->hwpwm / 2, SR));
	if (!(reg & ATMEL_TC_CLKSTA))
		__raw_writel(ATMEL_TC_CLKEN | ATMEL_TC_SWTRG, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CCR));
	spin_unlock(&tcbpwmc->lock);
	return 0;
}

static int atmel_tcb_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
		int duty_ns, int period_ns)
{
	struct atmel_tcb_pwm_chip *tcbpwmc = to_atmel_tcb_pwm_chip(chip);
//	struct atmel_tcb_pwm_device *tcbpwm = &(tcbpwmc->pwms[pwm->hwpwm]);
//	struct atmel_tcb_pwm_device *atcbpwm = &(tcbpwmc->pwms[pwm->hwpwm % 2 ? pwm->hwpwm - 1 : pwm->hwpwm + 1]);
	struct atmel_tc *tc = tcbpwmc->tc;
	int i;
	int bestclk;
	int slowclk = 0;
	u16 config;
	u32 period;
	u32 rate = clk_get_rate(tc->clk[pwm->hwpwm / 2]);
	u64 tmp = 1000000000;
	u64 min;
	u64 max;
	u32 save;
	u64 count;
	u32 reg;
	void __iomem *regs = tc->regs;
	u32 index = ATMEL_TC_REG(pwm->hwpwm / 2, CMR);


	for (i = 0; i < 5; ++i) {
		if (atmel_tc_divisors[i] == 0) {
			slowclk = i;
			continue;
		}

		min = div_u64 (1000000000 * atmel_tc_divisors[i], rate);

		max = min << tc->tcb_config->counter_width;
		if (max >= period_ns)
			break;
	}

	if (i == 5) {
		i = slowclk;
		rate = 32768;
		min = div_u64 (1000000000, rate);
		max = min << 16;

		if (max < period_ns)
			return -ERANGE;
	}

	count = div_u64 (duty_ns, min);
	count &=  ((u64)(1 << tc->tcb_config->counter_width) - 1);

	period = div_u64 (period_ns, min);
	period &= ((u64)(1 << tc->tcb_config->counter_width) - 1);

	spin_lock(&tcbpwmc->lock);
	reg = __raw_readl(regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));
	reg &= ~ATMEL_TC_TCCLKS;
	reg |= i;

	__raw_writel(reg, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CMR));

	__raw_writel(count, regs + ((pwm->hwpwm % 2) ? ATMEL_TC_REG(pwm->hwpwm / 2, RB) : ATMEL_TC_REG(pwm->hwpwm / 2, RA)));

	__raw_writel(tmp, regs + ATMEL_TC_REG(pwm->hwpwm / 2, RC));

	__raw_writel(0x4, regs + ATMEL_TC_REG(pwm->hwpwm / 2, CCR));

	spin_unlock(&tcbpwmc->lock);

	return 0;
}

static const struct pwm_ops atmel_tcb_pwm_ops = {
	.set_polarity = atmel_tcb_pwm_set_polarity,
	.request = atmel_tcb_pwm_request,
	.free = atmel_tcb_pwm_free,
	.config = atmel_tcb_pwm_config,
	.enable = atmel_tcb_pwm_enable,
	.disable = atmel_tcb_pwm_disable,
};

static int __devinit atmel_tcb_pwm_probe(struct platform_device *pdev)
{
	struct atmel_tcb_pwm_chip *tcbpwm;
	struct device_node *np = pdev->dev.of_node;
	struct atmel_tc *tc;
	int err;
	int tcblock;


	err = of_property_read_u32(np, "atmel,tc-block", &tcblock);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to get tc block number: %d\n", err);
		return err;
	}

	tc = atmel_tc_alloc(tcblock, "tcb-pwm");
	if (tc == NULL) {
		dev_err(&pdev->dev, "failed to allocate Timer Counter Block\n");
		return -ENOMEM;
	}

	tcbpwm = kzalloc(sizeof(*tcbpwm), GFP_KERNEL);
	if (tcbpwm == NULL) {
		dev_err(&pdev->dev, "failed to allocate memory\n");
		return -ENOMEM;
	}

	tcbpwm->chip.dev = &pdev->dev;
	tcbpwm->chip.ops = &atmel_tcb_pwm_ops;
	tcbpwm->chip.base = pdev->id;
	tcbpwm->chip.npwm = NPWM;
	tcbpwm->tc = tc;

	spin_lock_init(&tcbpwm->lock);

	err = pwmchip_add(&tcbpwm->chip);
	if (err < 0) {
		kfree(tcbpwm);
		return err;
	}

	dev_dbg(&pdev->dev, "pwm probe successful\n");
	platform_set_drvdata(pdev, tcbpwm);

	return 0;
}

static int __devexit atmel_tcb_pwm_remove(struct platform_device *pdev)
{
	struct atmel_tcb_pwm_chip *tcbpwm = platform_get_drvdata(pdev);
	int err;

	err = pwmchip_remove(&tcbpwm->chip);
	if (err < 0)
		return err;

	atmel_tc_free(tcbpwm->tc);

	dev_dbg(&pdev->dev, "pwm driver removed\n");
	kfree(tcbpwm);

	return 0;
}

static struct of_device_id atmel_tcb_pwm_dt_ids[] = {
	{ .compatible = "atmel,tcb-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mxs_pwm_dt_ids);

static struct platform_driver atmel_tcb_pwm_driver = {
	.driver = {
		.name = "atmel-tcb-pwm",
		.of_match_table = of_match_ptr(atmel_tcb_pwm_dt_ids),
	},
	.probe = atmel_tcb_pwm_probe,
	.remove = __devexit_p(atmel_tcb_pwm_remove),
};
module_platform_driver(atmel_tcb_pwm_driver);

MODULE_AUTHOR("Boris BREZILLON <b.brezillon@overkiz.com>");
MODULE_DESCRIPTION("Atmel Timer Counter Pulse Width Modulation Driver");
MODULE_ALIAS("platform:atmel-tcb-pwm");
MODULE_LICENSE("GPL v2");
