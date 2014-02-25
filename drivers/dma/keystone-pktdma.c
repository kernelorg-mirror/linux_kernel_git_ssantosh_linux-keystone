/*
 * Copyright (C) 2014 Texas Instruments Incorporated
 * Authors:	Sandeep Nair <sandeep_n@ti.com>
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

#include <linux/io.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/dma-direction.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/of_dma.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/keystone-pktdma.h>
#include <linux/pm_runtime.h>
#include <dt-bindings/dma/keystone.h>

#define BITS(x)			(BIT(x) - 1)
#define REG_MASK		0xffffffff

#define DMA_LOOPBACK		BIT(31)
#define DMA_ENABLE		BIT(31)
#define DMA_TEARDOWN		BIT(30)

#define DMA_TX_FILT_PSWORDS	BIT(29)
#define DMA_TX_FILT_EINFO	BIT(30)
#define DMA_TX_PRIO_SHIFT	0
#define DMA_RX_PRIO_SHIFT	16
#define DMA_PRIO_MASK		BITS(3)
#define DMA_PRIO_DEFAULT	0
#define DMA_RX_TIMEOUT_DEFAULT	17500 /* cycles */
#define DMA_RX_TIMEOUT_MASK	BITS(16)
#define DMA_RX_TIMEOUT_SHIFT	0

#define CHAN_HAS_EPIB		BIT(30)
#define CHAN_HAS_PSINFO		BIT(29)
#define CHAN_ERR_RETRY		BIT(28)
#define CHAN_PSINFO_AT_SOP	BIT(25)
#define CHAN_SOP_OFF_SHIFT	16
#define CHAN_SOP_OFF_MASK	BITS(9)
#define DESC_TYPE_SHIFT		26
#define DESC_TYPE_MASK		BITS(2)

/*
 * QMGR & QNUM together make up 14 bits with QMGR as the 2 MSb's in the logical
 * navigator cloud mapping scheme.
 * using the 14bit physical queue numbers directly maps into this scheme.
 */
#define CHAN_QNUM_MASK		BITS(14)
#define DMA_MAX_QMS		4
#define DMA_TIMEOUT		1000	/* msecs */

struct reg_global {
	u32	revision;
	u32	perf_control;
	u32	emulation_control;
	u32	priority_control;
	u32	qm_base_address[4];
};

struct reg_chan {
	u32	control;
	u32	mode;
	u32	__rsvd[6];
};

struct reg_tx_sched {
	u32	prio;
};

struct reg_rx_flow {
	u32	control;
	u32	tags;
	u32	tag_sel;
	u32	fdq_sel[2];
	u32	thresh[3];
};

#define BUILD_CHECK_REGS()						\
	do {								\
		BUILD_BUG_ON(sizeof(struct reg_global)   != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_chan)     != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_rx_flow)  != 32);	\
		BUILD_BUG_ON(sizeof(struct reg_tx_sched) !=  4);	\
	} while (0)

enum keystone_chan_state {
	/* stable states */
	CHAN_STATE_OPENED,
	CHAN_STATE_CLOSED,
};

struct keystone_dma_device {
	struct dma_device		engine;
	bool				loopback, enable_all;
	unsigned			tx_priority, rx_priority, rx_timeout;
	unsigned			logical_queue_managers;
	unsigned			qm_base_address[DMA_MAX_QMS];
	struct reg_global __iomem	*reg_global;
	struct reg_chan __iomem		*reg_tx_chan;
	struct reg_rx_flow __iomem	*reg_rx_flow;
	struct reg_chan __iomem		*reg_rx_chan;
	struct reg_tx_sched __iomem	*reg_tx_sched;
	unsigned			max_rx_chan, max_tx_chan;
	unsigned			max_rx_flow;
	atomic_t			in_use;
};
#define to_dma(dma)	(&(dma)->engine)
#define dma_dev(dma)	((dma)->engine.dev)

struct keystone_dma_chan {
	struct dma_chan			achan;
	enum dma_transfer_direction	direction;
	atomic_t			state;
	struct keystone_dma_device	*dma;

	/* registers */
	struct reg_chan __iomem		*reg_chan;
	struct reg_tx_sched __iomem	*reg_tx_sched;
	struct reg_rx_flow __iomem	*reg_rx_flow;

	/* configuration stuff */
	unsigned			channel, flow;
};
#define from_achan(ch)	container_of(ch, struct keystone_dma_chan, achan)
#define to_achan(ch)	(&(ch)->achan)
#define chan_dev(ch)	(&to_achan(ch)->dev->device)
#define chan_num(ch)	((ch->direction == DMA_MEM_TO_DEV) ? \
			ch->channel : ch->flow)
#define chan_vdbg(ch, format, arg...)				\
			dev_vdbg(chan_dev(ch), format, ##arg);

/**
 * dev_to_dma_chan - convert a device pointer to the its sysfs container object
 * @dev - device node
 */
static inline struct dma_chan *dev_to_dma_chan(struct device *dev)
{
	struct dma_chan_dev *chan_dev;

	chan_dev = container_of(dev, typeof(*chan_dev), device);
	return chan_dev->chan;
}

static inline enum keystone_chan_state
chan_get_state(struct keystone_dma_chan *chan)
{
	return atomic_read(&chan->state);
}

static int chan_start(struct keystone_dma_chan *chan,
			struct dma_keystone_cfg *cfg)
{
	u32 v = 0;

	if ((chan->direction == DMA_MEM_TO_DEV) && chan->reg_chan) {
		if (cfg->u.tx.filt_pswords)
			v |= DMA_TX_FILT_PSWORDS;
		if (cfg->u.tx.filt_einfo)
			v |= DMA_TX_FILT_EINFO;
		writel_relaxed(v, &chan->reg_chan->mode);
		writel_relaxed(DMA_ENABLE, &chan->reg_chan->control);
	}

	if (chan->reg_tx_sched)
		writel_relaxed(cfg->u.tx.priority, &chan->reg_tx_sched->prio);

	if (chan->reg_rx_flow) {
		v = 0;

		if (cfg->u.rx.einfo_present)
			v |= CHAN_HAS_EPIB;
		if (cfg->u.rx.psinfo_present)
			v |= CHAN_HAS_PSINFO;
		if (cfg->u.rx.err_mode == DMA_RETRY)
			v |= CHAN_ERR_RETRY;
		v |= (cfg->u.rx.desc_type & DESC_TYPE_MASK) << DESC_TYPE_SHIFT;
		if (cfg->u.rx.psinfo_at_sop)
			v |= CHAN_PSINFO_AT_SOP;
		v |= (cfg->u.rx.sop_offset & CHAN_SOP_OFF_MASK)
			<< CHAN_SOP_OFF_SHIFT;
		v |= cfg->u.rx.dst_q & CHAN_QNUM_MASK;

		writel_relaxed(v, &chan->reg_rx_flow->control);
		writel_relaxed(0, &chan->reg_rx_flow->tags);
		writel_relaxed(0, &chan->reg_rx_flow->tag_sel);

		v =  cfg->u.rx.fdq[0] << 16;
		v |=  cfg->u.rx.fdq[1] & CHAN_QNUM_MASK;
		writel_relaxed(v, &chan->reg_rx_flow->fdq_sel[0]);

		v =  cfg->u.rx.fdq[2] << 16;
		v |=  cfg->u.rx.fdq[3] & CHAN_QNUM_MASK;
		writel_relaxed(v, &chan->reg_rx_flow->fdq_sel[1]);

		writel_relaxed(0, &chan->reg_rx_flow->thresh[0]);
		writel_relaxed(0, &chan->reg_rx_flow->thresh[1]);
		writel_relaxed(0, &chan->reg_rx_flow->thresh[2]);
	}

	return 0;
}

static int chan_teardown(struct keystone_dma_chan *chan)
{
	unsigned long end, value;

	if (!chan->reg_chan)
		return 0;

	/* indicate teardown */
	writel_relaxed(DMA_TEARDOWN, &chan->reg_chan->control);

	/* wait for the dma to shut itself down */
	end = jiffies + msecs_to_jiffies(DMA_TIMEOUT);
	do {
		value = readl_relaxed(&chan->reg_chan->control);
		if ((value & DMA_ENABLE) == 0)
			break;
	} while (time_after(end, jiffies));

	if (readl_relaxed(&chan->reg_chan->control) & DMA_ENABLE) {
		dev_err(chan_dev(chan), "timeout waiting for teardown\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void chan_stop(struct keystone_dma_chan *chan)
{
	if (chan->reg_rx_flow) {
		/* first detach fdqs, starve out the flow */
		writel_relaxed(0, &chan->reg_rx_flow->fdq_sel[0]);
		writel_relaxed(0, &chan->reg_rx_flow->fdq_sel[1]);
		writel_relaxed(0, &chan->reg_rx_flow->thresh[0]);
		writel_relaxed(0, &chan->reg_rx_flow->thresh[1]);
		writel_relaxed(0, &chan->reg_rx_flow->thresh[2]);
	}

	/* teardown the dma channel */
	chan_teardown(chan);

	/* then disconnect the completion side */
	if (chan->reg_rx_flow) {
		writel_relaxed(0, &chan->reg_rx_flow->control);
		writel_relaxed(0, &chan->reg_rx_flow->tags);
		writel_relaxed(0, &chan->reg_rx_flow->tag_sel);
	}
	chan_vdbg(chan, "channel stopped\n");
}

static void keystone_dma_hw_init(struct keystone_dma_device *dma)
{
	unsigned v;
	int i;

	v  = dma->loopback ? DMA_LOOPBACK : 0;
	writel_relaxed(v, &dma->reg_global->emulation_control);

	v = readl_relaxed(&dma->reg_global->perf_control);
	v |= ((dma->rx_timeout & DMA_RX_TIMEOUT_MASK) << DMA_RX_TIMEOUT_SHIFT);
	writel_relaxed(v, &dma->reg_global->perf_control);

	v = ((dma->tx_priority << DMA_TX_PRIO_SHIFT) |
	     (dma->rx_priority << DMA_RX_PRIO_SHIFT));

	writel_relaxed(v, &dma->reg_global->priority_control);

	if (dma->enable_all) {
		for (i = 0; i < dma->max_tx_chan; i++) {
			writel_relaxed(0, &dma->reg_tx_chan[i].mode);
			writel_relaxed(DMA_ENABLE,
				       &dma->reg_tx_chan[i].control);
		}
	}

	/* Always enable all Rx channels. Rx paths are managed using flows */
	for (i = 0; i < dma->max_rx_chan; i++)
		writel_relaxed(DMA_ENABLE, &dma->reg_rx_chan[i].control);

	for (i = 0; i < dma->logical_queue_managers; i++)
		writel_relaxed(dma->qm_base_address[i],
			       &dma->reg_global->qm_base_address[i]);
}

static void keystone_dma_hw_destroy(struct keystone_dma_device *dma)
{
	int i;
	unsigned v;

	v = ~DMA_ENABLE & REG_MASK;

	for (i = 0; i < dma->max_rx_chan; i++)
		writel_relaxed(v, &dma->reg_rx_chan[i].control);

	for (i = 0; i < dma->max_tx_chan; i++)
		writel_relaxed(v, &dma->reg_tx_chan[i].control);
}

static int chan_init(struct dma_chan *achan)
{
	struct keystone_dma_chan *chan = from_achan(achan);
	struct keystone_dma_device *dma = chan->dma;

	chan_vdbg(chan, "initializing %s channel\n",
		  chan->direction == DMA_MEM_TO_DEV   ? "transmit" :
		  chan->direction == DMA_DEV_TO_MEM ? "receive"  :
		  "unknown");

	if (chan->direction != DMA_MEM_TO_DEV &&
	    chan->direction != DMA_DEV_TO_MEM) {
		dev_err(chan_dev(chan), "bad direction\n");
		return -EINVAL;
	}

	atomic_set(&chan->state, CHAN_STATE_OPENED);

	if (atomic_inc_return(&dma->in_use) <= 1)
		keystone_dma_hw_init(dma);

	return 0;
}

static void chan_destroy(struct dma_chan *achan)
{
	struct keystone_dma_chan *chan = from_achan(achan);
	struct keystone_dma_device *dma = chan->dma;

	if (chan_get_state(chan) == CHAN_STATE_CLOSED)
		return;

	chan_vdbg(chan, "destroying channel\n");
	chan_stop(chan);
	atomic_set(&chan->state, CHAN_STATE_CLOSED);
	if (atomic_dec_return(&dma->in_use) <= 0)
		keystone_dma_hw_destroy(dma);
	chan_vdbg(chan, "channel destroyed\n");
}

static int chan_keystone_config(struct keystone_dma_chan *chan,
		struct dma_keystone_cfg *cfg)
{
	if (chan_get_state(chan) != CHAN_STATE_OPENED)
		return -ENODEV;

	if (cfg->sl_cfg.direction != chan->direction)
		return -EINVAL;

	return chan_start(chan, cfg);
}

static int chan_control(struct dma_chan *achan, enum dma_ctrl_cmd cmd,
			unsigned long arg)
{
	struct keystone_dma_chan *chan = from_achan(achan);
	struct dma_keystone_cfg *keystone_config;
	struct dma_slave_config *dma_cfg;
	int ret;

	switch (cmd) {
	case DMA_SLAVE_CONFIG:
		dma_cfg = (struct dma_slave_config *)arg;
		keystone_config = keystone_cfg_from_slave_config(dma_cfg);
		ret = chan_keystone_config(chan, keystone_config);
		break;

	default:
		ret = -ENOTSUPP;
		break;
	}
	return ret;
}

static void __iomem *pktdma_get_regs(
		struct keystone_dma_device *dma, const char *name,
		resource_size_t *_size)
{
	struct device *dev = dma_dev(dma);
	struct device_node *node = dev->of_node;
	resource_size_t size;
	struct resource res;
	void __iomem *regs;
	int i;

	i = of_property_match_string(node, "reg-names", name);
	if (of_address_to_resource(node, i, &res)) {
		dev_err(dev, "could not find %s resource(index %d)\n", name, i);
		return NULL;
	}
	size = resource_size(&res);

	regs = of_iomap(node, i);
	if (!regs) {
		dev_err(dev, "could not map %s resource (index %d)\n", name, i);
		return NULL;
	}

	dev_dbg(dev, "index: %d, res:%s, size:%x, phys:%x, virt:%p\n",
		i, name, (unsigned int)size, (unsigned int)res.start, regs);

	if (_size)
		*_size = size;

	return regs;
}

static int pktdma_init_rx_chan(struct keystone_dma_chan *chan,
				      struct device_node *node,
				      u32 flow)
{
	struct keystone_dma_device *dma = chan->dma;
	struct device *dev = dma_dev(chan->dma);

	chan->flow = flow;
	chan->reg_rx_flow = dma->reg_rx_flow + flow;
	dev_dbg(dev, "rx flow(%d) (%p)\n", chan->flow, chan->reg_rx_flow);

	return 0;
}

static int pktdma_init_tx_chan(struct keystone_dma_chan *chan,
				struct device_node *node,
				u32 channel)
{
	struct keystone_dma_device *dma = chan->dma;
	struct device *dev = dma_dev(chan->dma);

	chan->channel = channel;
	chan->reg_chan = dma->reg_tx_chan + channel;
	chan->reg_tx_sched = dma->reg_tx_sched + channel;
	dev_dbg(dev, "tx channel(%d) (%p)\n", chan->channel, chan->reg_chan);

	return 0;
}

static int pktdma_init_chan(struct keystone_dma_device *dma,
				struct device_node *node,
				enum dma_transfer_direction dir,
				unsigned chan_num)
{
	struct device *dev = dma_dev(dma);
	struct keystone_dma_chan *chan;
	struct dma_chan *achan;
	int ret = -EINVAL;

	chan = devm_kzalloc(dev, sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	achan = to_achan(chan);
	achan->device   = &dma->engine;
	chan->dma	= dma;
	chan->direction	= DMA_NONE;
	atomic_set(&chan->state, CHAN_STATE_OPENED);

	if (dir == DMA_MEM_TO_DEV) {
		chan->direction = dir;
		ret = pktdma_init_tx_chan(chan, node, chan_num);
	} else if (dir == DMA_DEV_TO_MEM) {
		chan->direction = dir;
		ret = pktdma_init_rx_chan(chan, node, chan_num);
	} else {
		dev_err(dev, "channel(%d) direction unknown\n", chan_num);
	}

	if (ret < 0)
		goto fail;

	list_add_tail(&to_achan(chan)->device_node, &to_dma(dma)->channels);
	return 0;

fail:
	devm_kfree(dev, chan);
	return ret;
}

/* dummy function: feature not supported */
static enum dma_status chan_xfer_status(struct dma_chan *achan,
				      dma_cookie_t cookie,
				      struct dma_tx_state *txstate)
{
	WARN(1, "xfer status not supported\n");
	return DMA_ERROR;
}

/* dummy function: feature not supported */
static void chan_issue_pending(struct dma_chan *chan)
{
	WARN(1, "issue pending not supported\n");
}

static ssize_t keystone_dma_show_chan_num(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct dma_chan *achan = dev_to_dma_chan(dev);
	struct keystone_dma_chan *chan = from_achan(achan);

	return scnprintf(buf, PAGE_SIZE, "%u\n", chan->channel);
}

static ssize_t keystone_dma_show_flow(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct dma_chan *achan = dev_to_dma_chan(dev);
	struct keystone_dma_chan *chan = from_achan(achan);

	return scnprintf(buf, PAGE_SIZE, "%u\n", chan->flow);
}

static DEVICE_ATTR(chan_num, S_IRUSR, keystone_dma_show_chan_num, NULL);
static DEVICE_ATTR(rx_flow, S_IRUSR, keystone_dma_show_flow, NULL);

static void keystone_dma_destroy_attr(struct keystone_dma_device *dma)
{
	struct dma_device *engine = to_dma(dma);
	struct keystone_dma_chan *chan;
	struct dma_chan *achan;
	struct device *dev;

	list_for_each_entry(achan, &engine->channels, device_node) {
		chan = from_achan(achan);
		dev = chan_dev(chan);

		/* remove sysfs entries */
		if (chan->direction == DMA_MEM_TO_DEV)
			device_remove_file(dev, &dev_attr_chan_num);
		else
			device_remove_file(dev, &dev_attr_rx_flow);
	}
}

static int  keystone_dma_setup_attr(struct keystone_dma_device *dma)
{
	struct dma_device *engine = to_dma(dma);
	struct keystone_dma_chan *chan;
	struct dma_chan *achan;
	struct device *dev;
	int status = 0;

	list_for_each_entry(achan, &engine->channels, device_node) {
		chan = from_achan(achan);
		dev = chan_dev(chan);

		/* add sysfs entries */
		if (chan->direction == DMA_MEM_TO_DEV) {
			status = device_create_file(dev, &dev_attr_chan_num);
			if (status)
				dev_warn(dev,
					 "Couldn't create sysfs chan_num\n");
		} else {
			status = device_create_file(dev, &dev_attr_rx_flow);
			if (status)
				dev_warn(dev,
					 "Couldn't create sysfs rx_flow\n");
		}
	}
	return status;
}

static struct of_dma_filter_info keystone_dma_info = {
	.filter_fn = dma_keystone_filter_fn,
};

static int keystone_dma_probe(struct platform_device *pdev)
{
	unsigned max_tx_chan, max_rx_chan, max_rx_flow, max_tx_sched;
	struct device_node *node = pdev->dev.of_node;
	struct keystone_dma_device *dma;
	struct device_node *chan;
	int ret, len, num_chan = 0;
	struct dma_device *engine;
	resource_size_t size;
	u32 timeout;
	u32 i;

	if (!node) {
		dev_err(&pdev->dev, "could not find device info\n");
		return -EINVAL;
	}

	dma = devm_kzalloc(&pdev->dev, sizeof(*dma), GFP_KERNEL);
	if (!dma) {
		dev_err(&pdev->dev, "could not allocate driver mem\n");
		return -ENOMEM;
	}

	engine = to_dma(dma);
	engine->dev = &pdev->dev;
	platform_set_drvdata(pdev, dma);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to enable pktdma, err %d\n", ret);
		return ret;
	}

	dma->reg_global	 = pktdma_get_regs(dma, "global", &size);
	if (!dma->reg_global)
		return -ENODEV;
	if (size < sizeof(struct reg_global)) {
		dev_err(dma_dev(dma), "bad size %pa for global regs\n", &size);
		return -ENODEV;
	}

	dma->reg_tx_chan = pktdma_get_regs(dma, "txchan", &size);
	if (!dma->reg_tx_chan)
		return -ENODEV;

	max_tx_chan = size / sizeof(struct reg_chan);
	dma->reg_rx_chan = pktdma_get_regs(dma, "rxchan", &size);
	if (!dma->reg_rx_chan)
		return -ENODEV;

	max_rx_chan = size / sizeof(struct reg_chan);
	dma->reg_tx_sched = pktdma_get_regs(dma, "txsched", &size);
	if (!dma->reg_tx_sched)
		return -ENODEV;

	max_tx_sched = size / sizeof(struct reg_tx_sched);
	dma->reg_rx_flow = pktdma_get_regs(dma, "rxflow", &size);
	if (!dma->reg_rx_flow)
		return -ENODEV;

	max_rx_flow = size / sizeof(struct reg_rx_flow);
	dma->rx_priority = DMA_PRIO_DEFAULT;
	dma->tx_priority = DMA_PRIO_DEFAULT;

	dma->enable_all	= (of_get_property(node, "enable-all", NULL) != NULL);
	dma->loopback	= (of_get_property(node, "loop-back",  NULL) != NULL);

	ret = of_property_read_u32(node, "rx-retry-timeout", &timeout);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "unspecified rx timeout using value %d\n",
			DMA_RX_TIMEOUT_DEFAULT);
		timeout = DMA_RX_TIMEOUT_DEFAULT;
	}
	dma->rx_timeout = timeout;

	if (!of_find_property(node, "qm-base-address", &len)) {
		dev_err(&pdev->dev, "unspecified qm-base-address\n");
		return -ENODEV;
	}

	dma->logical_queue_managers = len / sizeof(u32);
	if (dma->logical_queue_managers > DMA_MAX_QMS) {
		dev_warn(&pdev->dev, "too many queue mgrs(>%d) rest ignored\n",
			 dma->logical_queue_managers);
		dma->logical_queue_managers = DMA_MAX_QMS;
	}

	ret = of_property_read_u32_array(node, "qm-base-address",
					dma->qm_base_address,
					dma->logical_queue_managers);
	if (ret) {
		dev_err(&pdev->dev, "invalid qm-base-address\n");
		return ret;
	}

	dma->max_rx_chan = max_rx_chan;
	dma->max_rx_flow = max_rx_flow;
	dma->max_tx_chan = min(max_tx_chan, max_tx_sched);

	atomic_set(&dma->in_use, 0);

	INIT_LIST_HEAD(&engine->channels);

	for (i = 0; i < dma->max_tx_chan; i++) {
		if (pktdma_init_chan(dma, chan, DMA_MEM_TO_DEV, i) >= 0)
			num_chan++;
	}

	for (i = 0; i < dma->max_rx_chan; i++) {
		if (pktdma_init_chan(dma, chan, DMA_DEV_TO_MEM, i) >= 0)
			num_chan++;
	}

	if (list_empty(&engine->channels)) {
		dev_err(dma_dev(dma), "no valid channels\n");
		return -ENODEV;
	}

	dma_cap_set(DMA_SLAVE, engine->cap_mask);
	engine->device_alloc_chan_resources = chan_init;
	engine->device_free_chan_resources  = chan_destroy;
	engine->device_control		    = chan_control;
	engine->device_tx_status	    = chan_xfer_status;
	engine->device_issue_pending	    = chan_issue_pending;

	ret = dma_async_device_register(engine);
	if (ret) {
		dev_err(&pdev->dev, "unable to register dma engine\n");
		return ret;
	}

	keystone_dma_info.dma_cap = engine->cap_mask;
	ret = of_dma_controller_register(node, of_dma_simple_xlate,
					&keystone_dma_info);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register dma controller\n");
		dma_async_device_unregister(engine);
		return ret;
	}

	ret = keystone_dma_setup_attr(dma);
	if (ret) {
		dev_err(&pdev->dev, "unable to setup device attr\n");
		return ret;
	}

	dev_info(dma_dev(dma), "registered %d logical channels, flows %d, tx chans: %d, rx chans: %d%s\n",
		 num_chan, dma->max_rx_flow, dma->max_tx_chan,
		 dma->max_rx_chan, dma->loopback ? ", loopback" : "");

	return 0;
}

static int keystone_dma_remove(struct platform_device *pdev)
{
	struct keystone_dma_device *dma = platform_get_drvdata(pdev);
	struct dma_device *engine = to_dma(dma);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	keystone_dma_destroy_attr(dma);
	dma_async_device_unregister(engine);

	return 0;
}

static struct of_device_id of_match[] = {
	{ .compatible = "ti,keystone-pktdma", },
	{},
};

MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver keystone_dma_driver = {
	.probe	= keystone_dma_probe,
	.remove	= keystone_dma_remove,
	.driver = {
		.name		= "keystone-pktdma",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match,
	},
};

bool dma_keystone_filter_fn(struct dma_chan *chan, void *param)
{
	if (chan->device->dev->driver == &keystone_dma_driver.driver) {
		struct keystone_dma_chan *kdma_chan = from_achan(chan);
		unsigned chan_id = *(u32 *)param & KEYSTONE_DMA_CHAN_ID_MASK;
		unsigned chan_type = *(u32 *)param >> KEYSTONE_DMA_TYPE_SHIFT;

		if (chan_type == KEYSTONE_DMA_TX_CHAN_TYPE &&
		    kdma_chan->direction == DMA_MEM_TO_DEV)
			return chan_id  == kdma_chan->channel;

		if (chan_type == KEYSTONE_DMA_RX_FLOW_TYPE &&
		    kdma_chan->direction == DMA_DEV_TO_MEM)
			return chan_id  == kdma_chan->flow;
	}
	return false;
}
EXPORT_SYMBOL_GPL(dma_keystone_filter_fn);

static int __init keystone_dma_init(void)
{
	BUILD_CHECK_REGS();
	return platform_driver_register(&keystone_dma_driver);
}
module_init(keystone_dma_init);

static void __exit keystone_dma_exit(void)
{
	platform_driver_unregister(&keystone_dma_driver);
}
module_exit(keystone_dma_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI Keystone Packet DMA driver");
