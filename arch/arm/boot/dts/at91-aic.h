/*
 * This header provides constants for most at91 AIC bindings.
 *
 * Copyright (C) 2013 Jean-Christophe PLAGNIOL-VILLARD <plagnioj@jcrosoft.com>
 *
 * GPLv2 only
 */

#ifndef __DT_BINDINGS_AT91_AIC_H__
#define __DT_BINDINGS_AT91_AIC_H__

#define AT91_AIC_LOW_TO_HIGH		(1 < 0)
#define AT91_AIC_HIGH_TO_LOW		(1 < 1)
#define AT91_AIC_HIGH			(1 < 2)
#define AT91_AIC_LOW			(1 < 3)

#define AT91_AIC_LOW_TO_HIGH_OR_HIGH_TO_LOW	(AT91_AIC_LOW_TO_HIGH | AT91_AIC_HIGH_TO_LOW)

#endif /* __DT_BINDINGS_AT91_AIC_H__ */
