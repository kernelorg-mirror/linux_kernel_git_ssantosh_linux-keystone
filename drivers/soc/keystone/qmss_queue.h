/*
 * Keystone QMSS driver internal header
 *
 * Copyright (C) 2014 Texas Instruments Incorporated - http://www.ti.com
 * Author:	Sandeep Nair <sandeep_n@ti.com>
 *		Cyril Chemparathy <cyril@ti.com>
 *		Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef __QMSS_QUEUE_H__
#define __QMSS_QUEUE_H__

#define BITS(x)		(BIT(x) - 1)

#define THRESH_GTE	BIT(7)
#define THRESH_LT	0

#define PDSP_CTRL_PC_MASK	0xffff0000
#define PDSP_CTRL_SOFT_RESET	BIT(0)
#define PDSP_CTRL_ENABLE	BIT(1)
#define PDSP_CTRL_RUNNING	BIT(15)

#define ACC_MAX_CHANNEL		48
#define ACC_DEFAULT_PERIOD	25 /* usecs */

#define ACC_CHANNEL_INT_BASE		2

#define ACC_LIST_ENTRY_TYPE		1
#define ACC_LIST_ENTRY_WORDS		(1 << ACC_LIST_ENTRY_TYPE)
#define ACC_LIST_ENTRY_QUEUE_IDX	0
#define ACC_LIST_ENTRY_DESC_IDX	(ACC_LIST_ENTRY_WORDS - 1)

#define ACC_CMD_DISABLE_CHANNEL	0x80
#define ACC_CMD_ENABLE_CHANNEL	0x81
#define ACC_CFG_MULTI_QUEUE		BIT(21)

#define ACC_INTD_OFFSET_EOI		(0x0010)
#define ACC_INTD_OFFSET_COUNT(ch)	(0x0300 + 4 * (ch))
#define ACC_INTD_OFFSET_STATUS(ch)	(0x0200 + 4 * ((ch) / 32))

#define RANGE_MAX_IRQS			64

enum kqmss_acc_result {
	ACC_RET_IDLE,
	ACC_RET_SUCCESS,
	ACC_RET_INVALID_COMMAND,
	ACC_RET_INVALID_CHANNEL,
	ACC_RET_INACTIVE_CHANNEL,
	ACC_RET_ACTIVE_CHANNEL,
	ACC_RET_INVALID_QUEUE,
	ACC_RET_INVALID_RET,
};

struct kqmss_reg_config {
	u32		revision;
	u32		__pad1;
	u32		divert;
	u32		link_ram_base0;
	u32		link_ram_size0;
	u32		link_ram_base1;
	u32		__pad2[2];
	u32		starvation[0];
};

struct kqmss_reg_region {
	u32		base;
	u32		start_index;
	u32		size_count;
	u32		__pad;
};

struct kqmss_reg_pdsp_regs {
	u32		control;
	u32		status;
	u32		cycle_count;
	u32		stall_count;
};

struct kqmss_reg_acc_command {
	u32		command;
	u32		queue_mask;
	u32		list_phys;
	u32		queue_num;
	u32		timer_config;
};

struct kqmss_link_ram_block {
	dma_addr_t	 phys;
	void		*virt;
	size_t		 size;
};

struct kqmss_acc_info {
	u32			 pdsp_id;
	u32			 start_channel;
	u32			 list_entries;
	u32			 pacing_mode;
	u32			 timer_count;
	int			 mem_size;
	int			 list_size;
	struct kqmss_pdsp_info	*pdsp;
};

struct kqmss_acc_channel {
	u32			channel;
	u32			list_index;
	u32			open_mask;
	u32			*list_cpu[2];
	dma_addr_t		list_dma[2];
	char			name[KQMSS_NAME_SIZE];
	atomic_t		retrigger_count;
};

struct kqmss_pdsp_info {
	const char					*name;
	struct kqmss_reg_pdsp_regs  __iomem		*regs;
	union {
		void __iomem				*command;
		struct kqmss_reg_acc_command __iomem	*acc_command;
		u32 __iomem				*qos_command;
	};
	void __iomem					*intd;
	u32 __iomem					*iram;
	const char					*firmware;
	u32						id;
	struct list_head				list;
};

struct kqmss_qmgr_info {
	unsigned			start_queue;
	unsigned			num_queues;
	struct kqmss_reg_config __iomem	*reg_config;
	struct kqmss_reg_region __iomem	*reg_region;
	struct kqmss_reg_queue __iomem	*reg_push, *reg_pop, *reg_peek;
	void __iomem			*reg_status;
	struct list_head		list;
};

#define KQMSS_NUM_LINKRAM	2
struct kqmss_device {
	struct device				*dev;
	unsigned				base_id;
	unsigned				num_queues;
	unsigned				num_queues_in_use;
	unsigned				inst_shift;
	struct kqmss_link_ram_block		link_rams[KQMSS_NUM_LINKRAM];
	void					*instances;
	struct list_head			regions;
	struct list_head			queue_ranges;
	struct list_head			pools;
	struct list_head			pdsps;
	struct list_head			qmgrs;
};

struct kqmss_range_ops {
	int	(*init_range)(struct kqmss_range_info *range);
	int	(*free_range)(struct kqmss_range_info *range);
	int	(*init_queue)(struct kqmss_range_info *range,
			      struct kqmss_queue_inst *inst);
	int	(*open_queue)(struct kqmss_range_info *range,
			      struct kqmss_queue_inst *inst, unsigned flags);
	int	(*close_queue)(struct kqmss_range_info *range,
			       struct kqmss_queue_inst *inst);
	int	(*set_notify)(struct kqmss_range_info *range,
			      struct kqmss_queue_inst *inst, bool enabled);
};

struct kqmss_irq_info {
	int	irq;
	u32	cpu_map;
};

struct kqmss_range_info {
	const char			*name;
	struct kqmss_device		*kdev;
	unsigned			queue_base;
	unsigned			num_queues;
	void				*queue_base_inst;
	unsigned			flags;
	struct list_head		list;
	struct kqmss_range_ops		*ops;
	struct kqmss_acc_info		acc_info;
	struct kqmss_acc_channel	*acc;
	unsigned			num_irqs;
	struct kqmss_irq_info		irqs[RANGE_MAX_IRQS];
};

#define RANGE_RESERVED		BIT(0)
#define RANGE_HAS_IRQ		BIT(1)
#define RANGE_HAS_ACCUMULATOR	BIT(2)
#define RANGE_MULTI_QUEUE	BIT(3)

#define for_each_region(kdev, region)				\
	list_for_each_entry(region, &kdev->regions, list)

#define first_region(kdev)					\
	list_first_entry(&kdev->regions, \
			struct kqmss_region, list)

#define for_each_queue_range(kdev, range)			\
	list_for_each_entry(range, &kdev->queue_ranges, list)

#define first_queue_range(kdev)					\
	list_first_entry(&kdev->queue_ranges, \
			struct kqmss_range_info, list)

#define for_each_pool(kdev, pool)				\
	list_for_each_entry(pool, &kdev->pools, list)

#define for_each_pdsp(kdev, pdsp)				\
	list_for_each_entry(pdsp, &kdev->pdsps, list)

#define for_each_qmgr(kdev, qmgr)				\
	list_for_each_entry(qmgr, &kdev->qmgrs, list)

static inline struct kqmss_pdsp_info *
kqmss_find_pdsp(struct kqmss_device *kdev, unsigned pdsp_id)
{
	struct kqmss_pdsp_info *pdsp;

	for_each_pdsp(kdev, pdsp)
		if (pdsp_id == pdsp->id)
			return pdsp;
	return NULL;
}

#endif /* __QMSS_QUEUE_H__ */
