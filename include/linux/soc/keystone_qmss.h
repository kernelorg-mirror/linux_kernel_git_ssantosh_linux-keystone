/*
 * Keystone Queue Management Sub-System header
 *
 * Copyright (C) 2014 Texas Instruments Incorporated - http://www.ti.com
 * Author:	Sandeep Nair <sandeep_n@ti.com>
 *		Cyril Chemparathy <cyril@ti.com>
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

#ifndef __KEYSTONE_QMSS_H__
#define __KEYSTONE_QMSS_H__

#include <linux/err.h>
#include <linux/time.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/dma-mapping.h>

#define KQMSS_ACC_DESCS_MAX		SZ_1K
#define KQMSS_ACC_DESCS_MASK		(KQMSS_ACC_DESCS_MAX - 1)
#define KQMSS_DESC_SIZE_MASK		0xful
#define KQMSS_DESC_PTR_MASK		(~KQMSS_DESC_SIZE_MASK)
#define KQMSS_NAME_SIZE			32

/* queue types */
#define KQMSS_QUEUE_QPEND	((unsigned)-2) /* interruptible qpend queue */
#define KQMSS_QUEUE_ACC		((unsigned)-3) /* Accumulated queue */
#define KQMSS_QUEUE_GP		((unsigned)-4) /* General purpose queue */

/* queue flags */
#define KQMSS_QUEUE_SHARED	0x0001		/* Queue can be shared */

/* Queue notifier callback prototype */
typedef void (*kqmss_queue_notify_fn)(void *arg);

struct kqmss_device;
struct kqmss_acc_channel;
struct kqmss_reg_config;
struct kqmss_reg_region;
struct kqmss_qmgr_info;

/**
 * enum kqmss_queue_ctrl_cmd -	queue operations.
 * @KQMSS_QUEUE_GET_ID:		Get the ID number for an open queue
 * @KQMSS_QUEUE_FLUSH:		forcibly empty a queue if possible
 * @KQMSS_QUEUE_SET_NOTIFIER:	Set a notifier callback to a queue handle.
 * @KQMSS_QUEUE_ENABLE_NOTIFY:	Enable notifier callback for a queue handle.
 * @KQMSS_QUEUE_DISABLE_NOTIFY:	Disable notifier callback for a queue handle.
 */
enum kqmss_queue_ctrl_cmd {
	KQMSS_QUEUE_GET_ID,
	KQMSS_QUEUE_FLUSH,
	KQMSS_QUEUE_SET_NOTIFIER,
	KQMSS_QUEUE_ENABLE_NOTIFY,
	KQMSS_QUEUE_DISABLE_NOTIFY
};

/**
 * struct kqmss_queue_stats:	queue statistics
 * pushes:			number of push operations
 * pops:			number of pop operations
 * push_errors:			number of push errors
 * pop_errors:			number of pop errors
 * notifies:			notifier counts
 */
struct kqmss_queue_stats {
	atomic_t	 pushes;
	atomic_t	 pops;
	atomic_t	 push_errors;
	atomic_t	 pop_errors;
	atomic_t	 notifies;
};

/**
 * struct kqmss_reg_queue:	queue registers
 * @entry_count:		valid entries in the queue
 * @byte_count:			total byte count in thhe queue
 * @packet_size:		packet size for the queue
 * @ptr_size_thresh:		packet pointer size threshold
 */
struct kqmss_reg_queue {
	u32		entry_count;
	u32		byte_count;
	u32		packet_size;
	u32		ptr_size_thresh;
};

/**
 * struct kqmss_region:		qmss region info
 * @dma_start, dma_end:		start and end dma address
 * @virt_start, virt_end:	start and end virtual address
 * @desc_size:			descriptor size
 * @used_desc:			consumed descriptors
 * @id:				region number
 * @num_desc:			total descriptors
 * @link_index:			index of the first descriptor
 * @name:			region name
 * @list:			list head
 */
struct kqmss_region {
	dma_addr_t		dma_start, dma_end;
	void			*virt_start, *virt_end;
	unsigned		desc_size;
	unsigned		used_desc;
	unsigned		id;
	unsigned		num_desc;
	unsigned		link_index;
	const char		*name;
	struct list_head	list;
};

/**
 * struct kqmss_pool:		qmss pools
 * @dev:			device pointer
 * @region:			qmss region info
 * @queue:			queue registers
 * @kdev:			qmss device pointer
 * @region_offset:		offset from the base
 * @num_desc:			total descriptors
 * @desc_size:			descriptor size
 * @region_id:			region number
 * @name:			pool name
 * @list:			list head
 */
struct kqmss_pool {
	struct device			*dev;
	struct kqmss_region		*region;
	struct kqmss_queue		*queue;
	struct kqmss_device		*kdev;
	int				region_offset;
	int				num_desc;
	int				desc_size;
	int				region_id;
	const char			*name;
	struct list_head		list;
};

/**
 * struct kqmss_queue_inst:		qmss queue instace properties
 * @descs:				descriptor pointer
 * @desc_head, desc_tail, desc_count:	descriptor counters
 * @acc:				accumulator channel pointer
 * @kdev:				qmss device pointer
 * @range:				range info
 * @qmgr:				queue manager info
 * @id:					queue instace id
 * @irq_num:				irq line number
 * @notify_needed:			notifier needed based on queue type
 * @num_notifiers:			total notifiers
 * @handles:				list head
 * @name:				queue instance name
 * @irq_name:				irq line name
 */
struct kqmss_queue_inst {
	u32				*descs;
	atomic_t			desc_head, desc_tail, desc_count;
	struct kqmss_acc_channel	*acc;
	struct kqmss_device		*kdev;
	struct kqmss_range_info		*range;
	struct kqmss_qmgr_info		*qmgr;
	u32				id;
	int				irq_num;
	int				notify_needed;
	atomic_t			num_notifiers;
	struct list_head		handles;
	const char			*name;
	const char			*irq_name;
};

/**
 * struct kqmss_queue:			qmss queue properties
 * @reg_push, reg_pop, reg_peek:	push, pop queue registers
 * @inst:				qmss queue instace properties
 * @notifier_fn:			notifier function
 * @notifier_fn_arg:			notifier function argument
 * @notifier_enabled:			notier enabled for a give queue
 * @rcu:				rcu head
 * @flags:				queue flags
 * @list:				list head
 */
struct kqmss_queue {
	struct kqmss_reg_queue __iomem	*reg_push, *reg_pop, *reg_peek;
	struct kqmss_queue_inst		*inst;
	struct kqmss_queue_stats	stats;
	kqmss_queue_notify_fn		notifier_fn;
	void				*notifier_fn_arg;
	atomic_t			notifier_enabled;
	struct rcu_head			rcu;
	unsigned			flags;
	struct list_head		list;
};

/**
 * struct kqmss_queue_notify_config:	Notifier configuration
 * @fn:					Notifier function
 * @fn_arg:				Notifier function arguments
 */
struct kqmss_queue_notify_config {
	kqmss_queue_notify_fn fn;
	void *fn_arg;
};

/* Get the DMA address of a descriptor */
#define kqmss_pool_desc_virt_to_dma(pool, virt) \
	(pool->region->dma_start + (virt - pool->region->virt_start))

/* Get the virtual(cpu) address of a descriptor */
#define kqmss_pool_desc_dma_to_virt(pool, dma) \
	(pool->region->virt_start + (dma - pool->region->dma_start))

int kqmss_init_acc_range(struct kqmss_device *kdev, struct device_node *node,
					struct kqmss_range_info *range);
struct kqmss_queue *kqmss_queue_open(const char *name, unsigned id,
					unsigned flags);
void kqmss_queue_close(struct kqmss_queue *queue);
void kqmss_queue_notify(struct kqmss_queue_inst *inst);
int kqmss_queue_device_control(struct kqmss_queue *qh,
			enum kqmss_queue_ctrl_cmd cmd, unsigned long arg);

/**
 * kqmss_queue_get_count()	- returns number of elements in a queue
 * @qh				- hardware queue handle
 *
 * Returns number of elements in the queue.
 */
static inline int kqmss_queue_get_count(struct kqmss_queue *qh)
{
	struct kqmss_queue_inst *inst = qh->inst;

	return readl_relaxed(&qh->reg_peek[0].entry_count) +
		atomic_read(&inst->desc_count);
}

/**
 * kqmss_queue_push()	- push data (or descriptor) to the tail of a queue
 * @qh			- hardware queue handle
 * @data		- data to push
 * @size		- size of data to push
 * @flags		- can be used to pass additional information
 *
 * Returns 0 on success, errno otherwise.
 */
static inline int kqmss_queue_push(struct kqmss_queue *qh, dma_addr_t dma,
					unsigned size, unsigned flags)
{
	u32 val;

	val = (u32)dma | ((size / 16) - 1);
	writel_relaxed(val, &qh->reg_push[0].ptr_size_thresh);
	atomic_inc(&qh->stats.pushes);
	return 0;
}

/**
 * kqmss_queue_pop()	- pop data (or descriptor) from the head of a queue
 * @qh			- hardware queue handle
 * @size		- (optional) size of the data pop'ed.
 *
 * Returns a DMA address on success, 0 on failure.
 */
static inline dma_addr_t kqmss_queue_pop(struct kqmss_queue *qh, unsigned *size)
{
	struct kqmss_queue_inst *inst = qh->inst;
	dma_addr_t dma;
	u32 val, idx;

	/* are we accumulated? */
	if (inst->descs) {
		if (unlikely(atomic_dec_return(&inst->desc_count) < 0)) {
			atomic_inc(&inst->desc_count);
			return 0;
		}
		idx  = atomic_inc_return(&inst->desc_head);
		idx &= KQMSS_ACC_DESCS_MASK;
		val = inst->descs[idx];
	} else {
		val = readl_relaxed(&qh->reg_pop[0].ptr_size_thresh);
		if (unlikely(!val))
			return 0;
	}

	dma = val & KQMSS_DESC_PTR_MASK;
	if (size)
		*size = ((val & KQMSS_DESC_SIZE_MASK) + 1) * 16;

	atomic_inc(&qh->stats.pops);
	return dma;
}

struct kqmss_pool *kqmss_pool_create(const char *name,
					int num_desc, int region_id);
void kqmss_pool_destroy(struct kqmss_pool *pool);

/**
 * kqmss_pool_desc_get()	- Get a descriptor from the pool
 * @pool			- pool handle
 *
 * Returns descriptor from the pool.
 */
static inline void *kqmss_pool_desc_get(struct kqmss_pool *pool)
{
	dma_addr_t dma;
	unsigned size;
	void *data;

	dma = kqmss_queue_pop(pool->queue, &size);
	if (unlikely(!dma))
		return ERR_PTR(-ENOMEM);
	data = kqmss_pool_desc_dma_to_virt(pool, dma);
	return data;
}

/**
 * kqmss_pool_desc_put()	- return a descriptor to the pool
 * @pool			- pool handle
 */
static inline void kqmss_pool_desc_put(struct kqmss_pool *pool,
					void *desc)
{
	dma_addr_t dma;
	dma = kqmss_pool_desc_virt_to_dma(pool, desc);
	kqmss_queue_push(pool->queue, dma, pool->region->desc_size, 0);
}

/**
 * kqmss_pool_desc_map()	- Map descriptor for DMA transfer
 * @pool			- pool handle
 * @desc			- address of descriptor to map
 * @size			- size of descriptor to map
 * @dma				- DMA address return pointer
 * @dma_sz			- adjusted return pointer
 *
 * Returns 0 on success, errno otherwise.
 */
static inline int kqmss_pool_desc_map(struct kqmss_pool *pool,
					void *desc, unsigned size,
					dma_addr_t *dma, unsigned *dma_sz)
{
	*dma = kqmss_pool_desc_virt_to_dma(pool, desc);
	size = min(size, pool->region->desc_size);
	size = ALIGN(size, SMP_CACHE_BYTES);
	*dma_sz = size;
	dma_sync_single_for_device(pool->dev, *dma, size, DMA_TO_DEVICE);
	return 0;
}

/**
 * kqmss_pool_desc_unmap()	- Unmap descriptor after DMA transfer
 * @pool			- pool handle
 * @dma				- DMA address of descriptor to unmap
 * @dma_sz			- size of descriptor to unmap
 *
 * Returns descriptor address on success, Use IS_ERR_OR_NULL() to identify
 * error values on return.
 */
static inline void *kqmss_pool_desc_unmap(struct kqmss_pool *pool,
						dma_addr_t dma, unsigned dma_sz)
{
	unsigned desc_sz;
	void *desc;

	desc_sz = min(dma_sz, pool->region->desc_size);
	desc = kqmss_pool_desc_dma_to_virt(pool, dma);
	dma_sync_single_for_cpu(pool->dev, dma, desc_sz,
				DMA_FROM_DEVICE);
	prefetch(desc);
	return desc;
}

/**
 * kqmss_pool_count()	- Get the number of descriptors in pool.
 * @pool		- pool handle
 * Returns number of elements in the pool.
 */
static inline int kqmss_pool_count(struct kqmss_pool *pool)
{
	return kqmss_queue_get_count(pool->queue);
}

#endif /* __KEYSTONE_QMSS_H__ */
