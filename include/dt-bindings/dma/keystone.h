/*
 * Copyright (C) 2014 Texas Instruments Incorporated
 * Authors:	Sandeep Nair <sandeep_n@ti.com>
 *		Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __DT_BINDINGS_KEYSTONE_DMA_H__
#define __DT_BINDINGS_KEYSTONE_DMA_H__

#define KEYSTONE_DMA_CHAN_ID_MASK	(0xff)
#define KEYSTONE_DMA_TYPE_SHIFT		(24)
#define KEYSTONE_DMA_TX_CHAN_TYPE	(0xff)
#define KEYSTONE_DMA_RX_FLOW_TYPE	(0xfe)

#define KEYSTONE_DMA_TX_CHAN(id)	((id & KEYSTONE_DMA_CHAN_ID_MASK) | \
					(KEYSTONE_DMA_TX_CHAN_TYPE << \
					 KEYSTONE_DMA_TYPE_SHIFT))

#define KEYSTONE_DMA_RX_FLOW(id)	((id & KEYSTONE_DMA_CHAN_ID_MASK) | \
					(KEYSTONE_DMA_RX_FLOW_TYPE << \
					 KEYSTONE_DMA_TYPE_SHIFT))


#endif /* __DT_BINDINGS_KEYSTONE_DMA_H_ */
