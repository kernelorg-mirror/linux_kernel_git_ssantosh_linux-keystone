/*
 *    Copyright (C) 2006 Benjamin Herrenschmidt, IBM Corp.
 *			 <benh@kernel.crashing.org>
 *    and		 Arnd Bergmann, IBM Corp.
 *    Merged from powerpc/kernel/of_platform.c and
 *    sparc{,64}/kernel/of_device.c by Stephen Rothwell
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/amba/bus.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

const struct of_device_id of_default_bus_match_table[] = {
	{ .compatible = "simple-bus", },
#ifdef CONFIG_ARM_AMBA
	{ .compatible = "arm,amba-bus", },
#endif /* CONFIG_ARM_AMBA */
	{} /* Empty terminated list */
};

static int of_dev_node_match(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/**
 * of_find_device_by_node - Find the platform_device associated with a node
 * @np: Pointer to device tree node
 *
 * Returns platform_device pointer, or NULL if not found
 */
struct platform_device *of_find_device_by_node(struct device_node *np)
{
	struct device *dev;

	dev = bus_find_device(&platform_bus_type, NULL, np, of_dev_node_match);
	return dev ? to_platform_device(dev) : NULL;
}
EXPORT_SYMBOL(of_find_device_by_node);

#if defined(CONFIG_PPC_DCR)
#include <asm/dcr.h>
#endif

#ifdef CONFIG_OF_ADDRESS
/*
 * The following routines scan a subtree and registers a device for
 * each applicable node.
 *
 * Note: sparc doesn't use these routines because it has a different
 * mechanism for creating devices from device tree nodes.
 */

/**
 * of_device_make_bus_id - Use the device node data to assign a unique name
 * @dev: pointer to device structure that is linked to a device tree node
 *
 * This routine will first try using either the dcr-reg or the reg property
 * value to derive a unique name.  As a last resort it will use the node
 * name followed by a unique number.
 */
void of_device_make_bus_id(struct device *dev)
{
	static atomic_t bus_no_reg_magic;
	struct device_node *node = dev->of_node;
	const __be32 *reg;
	u64 addr;
	const __be32 *addrp;
	int magic;

#ifdef CONFIG_PPC_DCR
	/*
	 * If it's a DCR based device, use 'd' for native DCRs
	 * and 'D' for MMIO DCRs.
	 */
	reg = of_get_property(node, "dcr-reg", NULL);
	if (reg) {
#ifdef CONFIG_PPC_DCR_NATIVE
		dev_set_name(dev, "d%x.%s", *reg, node->name);
#else /* CONFIG_PPC_DCR_NATIVE */
		u64 addr = of_translate_dcr_address(node, *reg, NULL);
		if (addr != OF_BAD_ADDR) {
			dev_set_name(dev, "D%llx.%s",
				     (unsigned long long)addr, node->name);
			return;
		}
#endif /* !CONFIG_PPC_DCR_NATIVE */
	}
#endif /* CONFIG_PPC_DCR */

	/*
	 * For MMIO, get the physical address
	 */
	reg = of_get_property(node, "reg", NULL);
	if (reg) {
		if (of_can_translate_address(node)) {
			addr = of_translate_address(node, reg);
		} else {
			addrp = of_get_address(node, 0, NULL, NULL);
			if (addrp)
				addr = of_read_number(addrp, 1);
			else
				addr = OF_BAD_ADDR;
		}
		if (addr != OF_BAD_ADDR) {
			dev_set_name(dev, "%llx.%s",
				     (unsigned long long)addr, node->name);
			return;
		}
	}

	/*
	 * No BusID, use the node name and add a globally incremented
	 * counter (and pray...)
	 */
	magic = atomic_add_return(1, &bus_no_reg_magic);
	dev_set_name(dev, "%s.%d", node->name, magic - 1);
}

/**
 * of_device_alloc - Allocate and initialize an of_device
 * @np: device node to assign to device
 * @bus_id: Name to assign to the device.  May be null to use default name.
 * @parent: Parent device.
 */
struct platform_device *of_device_alloc(struct device_node *np,
				  const char *bus_id,
				  struct device *parent)
{
	struct platform_device *dev;
	int rc, i, num_reg = 0, num_irq;
	struct resource *res, temp_res;

	dev = platform_device_alloc("", -1);
	if (!dev)
		return NULL;

	/* count the io and irq resources */
	if (of_can_translate_address(np))
		while (of_address_to_resource(np, num_reg, &temp_res) == 0)
			num_reg++;
	num_irq = of_irq_count(np);

	/* Populate the resource table */
	if (num_irq || num_reg) {
		res = kzalloc(sizeof(*res) * (num_irq + num_reg), GFP_KERNEL);
		if (!res) {
			platform_device_put(dev);
			return NULL;
		}

		dev->num_resources = num_reg + num_irq;
		dev->resource = res;
		for (i = 0; i < num_reg; i++, res++) {
			rc = of_address_to_resource(np, i, res);
			WARN_ON(rc);
		}
		WARN_ON(of_irq_to_resource_table(np, res, num_irq) != num_irq);
	}

	dev->dev.of_node = of_node_get(np);
#if defined(CONFIG_MICROBLAZE)
	dev->dev.dma_mask = &dev->archdata.dma_mask;
#endif
	dev->dev.parent = parent;

	if (bus_id)
		dev_set_name(&dev->dev, "%s", bus_id);
	else
		of_device_make_bus_id(&dev->dev);

	return dev;
}
EXPORT_SYMBOL(of_device_alloc);

/**
 * dt_dma_configure - apply default DMA configuration from dt
 * @dev:	Device to apply DMA configuration
 *
 * Try to get devices's DMA configuration from DT and apply it.
 * The DMA configuration is represented in DT by "dma-ranges" property.
 * It configures:
 *	dma_pfn_offset, dma_mask and coherent_dma_mask.
 *
 * In case if DMA configuration can't be acquired from DT the default one is
 * applied:
 *	dma_pfn_offset = 0,
 *	dma_mask = DMA_BIT_MASK(32)
 *	coherent_dma_mask = DMA_BIT_MASK(32).
 *
 * In case if platform code need to use own DMA configuration - it can use
 * Platform bus notifier and handle BUS_NOTIFY_ADD_DEVICE event to fix up
 * DMA configuration.
 */
static void dt_dma_configure(struct device *dev)
{
	dma_addr_t dma_addr;
	phys_addr_t paddr, size;
	dma_addr_t dma_mask;
	int ret;

	/*
	 * if dma-ranges property doesn't exist - use 32 bits DMA mask
	 * by default and don't set skip archdata.dma_pfn_offset
	 */
	ret = of_dma_get_range(dev->of_node, &dma_addr, &paddr, &size);
	if (ret == -ENODEV) {
		dev->coherent_dma_mask = DMA_BIT_MASK(32);
		if (!dev->dma_mask)
			dev->dma_mask = &dev->coherent_dma_mask;
		return;
	}

	/* if failed - disable DMA for device */
	if (ret < 0) {
		dev_err(dev, "failed to configure DMA\n");
		return;
	}

	/* DMA ranges found. Calculate and set dma_pfn_offset */
	dev->dma_pfn_offset = PFN_DOWN(paddr - dma_addr);

	/* Configure DMA mask */
	dev->dma_mask = &dev->coherent_dma_mask;
	if (!dev->dma_mask)
		return;

	dma_mask = dma_addr + size - 1;

	ret = dma_set_mask(dev, dma_mask);
	if (ret < 0) {
		dev_err(dev, "failed to set DMA mask %pad\n", &dma_mask);
		dev->dma_mask = NULL;
		return;
	}

	dev_dbg(dev, "dma_pfn_offset(%#08lx) dma_mask(%pad)\n",
		dev->dma_pfn_offset, dev->dma_mask);

	ret = dma_set_coherent_mask(dev, dma_mask);
	if (ret < 0) {
		dev_err(dev, "failed to set coherent DMA mask %pad\n",
			&dma_mask);
	}
}

/**
 * of_platform_device_create_pdata - Alloc, initialize and register an of_device
 * @np: pointer to node to create device for
 * @bus_id: name to assign device
 * @platform_data: pointer to populate platform_data pointer with
 * @parent: Linux device model parent device.
 *
 * Returns pointer to created platform device, or NULL if a device was not
 * registered.  Unavailable devices will not get registered.
 */
static struct platform_device *of_platform_device_create_pdata(
					struct device_node *np,
					const char *bus_id,
					void *platform_data,
					struct device *parent)
{
	struct platform_device *dev;

	if (!of_device_is_available(np))
		return NULL;

	dev = of_device_alloc(np, bus_id, parent);
	if (!dev)
		return NULL;

#if defined(CONFIG_MICROBLAZE)
	dev->archdata.dma_mask = 0xffffffffUL;
#endif
	dt_dma_configure(&dev->dev);
	dev->dev.bus = &platform_bus_type;
	dev->dev.platform_data = platform_data;

	/* We do not fill the DMA ops for platform devices by default.
	 * This is currently the responsibility of the platform code
	 * to do such, possibly using a device notifier
	 */

	if (of_device_add(dev) != 0) {
		platform_device_put(dev);
		return NULL;
	}

	return dev;
}

/**
 * of_platform_device_create - Alloc, initialize and register an of_device
 * @np: pointer to node to create device for
 * @bus_id: name to assign device
 * @parent: Linux device model parent device.
 *
 * Returns pointer to created platform device, or NULL if a device was not
 * registered.  Unavailable devices will not get registered.
 */
struct platform_device *of_platform_device_create(struct device_node *np,
					    const char *bus_id,
					    struct device *parent)
{
	return of_platform_device_create_pdata(np, bus_id, NULL, parent);
}
EXPORT_SYMBOL(of_platform_device_create);

#ifdef CONFIG_ARM_AMBA
static struct amba_device *of_amba_device_create(struct device_node *node,
						 const char *bus_id,
						 void *platform_data,
						 struct device *parent)
{
	struct amba_device *dev;
	const void *prop;
	int i, ret;

	pr_debug("Creating amba device %s\n", node->full_name);

	if (!of_device_is_available(node))
		return NULL;

	dev = amba_device_alloc(NULL, 0, 0);
	if (!dev) {
		pr_err("%s(): amba_device_alloc() failed for %s\n",
		       __func__, node->full_name);
		return NULL;
	}

	/* setup generic device info */
	dev->dev.coherent_dma_mask = ~0;
	dev->dev.of_node = of_node_get(node);
	dev->dev.parent = parent;
	dev->dev.platform_data = platform_data;
	if (bus_id)
		dev_set_name(&dev->dev, "%s", bus_id);
	else
		of_device_make_bus_id(&dev->dev);

	/* Allow the HW Peripheral ID to be overridden */
	prop = of_get_property(node, "arm,primecell-periphid", NULL);
	if (prop)
		dev->periphid = of_read_ulong(prop, 1);

	/* Decode the IRQs and address ranges */
	for (i = 0; i < AMBA_NR_IRQS; i++)
		dev->irq[i] = irq_of_parse_and_map(node, i);

	ret = of_address_to_resource(node, 0, &dev->res);
	if (ret) {
		pr_err("%s(): of_address_to_resource() failed (%d) for %s\n",
		       __func__, ret, node->full_name);
		goto err_free;
	}

	ret = amba_device_add(dev, &iomem_resource);
	if (ret) {
		pr_err("%s(): amba_device_add() failed (%d) for %s\n",
		       __func__, ret, node->full_name);
		goto err_free;
	}

	return dev;

err_free:
	amba_device_put(dev);
	return NULL;
}
#else /* CONFIG_ARM_AMBA */
static struct amba_device *of_amba_device_create(struct device_node *node,
						 const char *bus_id,
						 void *platform_data,
						 struct device *parent)
{
	return NULL;
}
#endif /* CONFIG_ARM_AMBA */

/**
 * of_devname_lookup() - Given a device node, lookup the preferred Linux name
 */
static const struct of_dev_auxdata *of_dev_lookup(const struct of_dev_auxdata *lookup,
				 struct device_node *np)
{
	struct resource res;

	if (!lookup)
		return NULL;

	for(; lookup->compatible != NULL; lookup++) {
		if (!of_device_is_compatible(np, lookup->compatible))
			continue;
		if (!of_address_to_resource(np, 0, &res))
			if (res.start != lookup->phys_addr)
				continue;
		pr_debug("%s: devname=%s\n", np->full_name, lookup->name);
		return lookup;
	}

	return NULL;
}

/**
 * of_platform_bus_create() - Create a device for a node and its children.
 * @bus: device node of the bus to instantiate
 * @matches: match table for bus nodes
 * @lookup: auxdata table for matching id and platform_data with device nodes
 * @parent: parent for new device, or NULL for top level.
 * @strict: require compatible property
 *
 * Creates a platform_device for the provided device_node, and optionally
 * recursively create devices for all the child nodes.
 */
static int of_platform_bus_create(struct device_node *bus,
				  const struct of_device_id *matches,
				  const struct of_dev_auxdata *lookup,
				  struct device *parent, bool strict)
{
	const struct of_dev_auxdata *auxdata;
	struct device_node *child;
	struct platform_device *dev;
	const char *bus_id = NULL;
	void *platform_data = NULL;
	int rc = 0;

	/* Make sure it has a compatible property */
	if (strict && (!of_get_property(bus, "compatible", NULL))) {
		pr_debug("%s() - skipping %s, no compatible prop\n",
			 __func__, bus->full_name);
		return 0;
	}

	auxdata = of_dev_lookup(lookup, bus);
	if (auxdata) {
		bus_id = auxdata->name;
		platform_data = auxdata->platform_data;
	}

	if (of_device_is_compatible(bus, "arm,primecell")) {
		/*
		 * Don't return an error here to keep compatibility with older
		 * device tree files.
		 */
		of_amba_device_create(bus, bus_id, platform_data, parent);
		return 0;
	}

	dev = of_platform_device_create_pdata(bus, bus_id, platform_data, parent);
	if (!dev || !of_match_node(matches, bus))
		return 0;

	for_each_child_of_node(bus, child) {
		pr_debug("   create child: %s\n", child->full_name);
		rc = of_platform_bus_create(child, matches, lookup, &dev->dev, strict);
		if (rc) {
			of_node_put(child);
			break;
		}
	}
	return rc;
}

/**
 * of_platform_bus_probe() - Probe the device-tree for platform buses
 * @root: parent of the first level to probe or NULL for the root of the tree
 * @matches: match table for bus nodes
 * @parent: parent to hook devices from, NULL for toplevel
 *
 * Note that children of the provided root are not instantiated as devices
 * unless the specified root itself matches the bus list and is not NULL.
 */
int of_platform_bus_probe(struct device_node *root,
			  const struct of_device_id *matches,
			  struct device *parent)
{
	struct device_node *child;
	int rc = 0;

	root = root ? of_node_get(root) : of_find_node_by_path("/");
	if (!root)
		return -EINVAL;

	pr_debug("of_platform_bus_probe()\n");
	pr_debug(" starting at: %s\n", root->full_name);

	/* Do a self check of bus type, if there's a match, create children */
	if (of_match_node(matches, root)) {
		rc = of_platform_bus_create(root, matches, NULL, parent, false);
	} else for_each_child_of_node(root, child) {
		if (!of_match_node(matches, child))
			continue;
		rc = of_platform_bus_create(child, matches, NULL, parent, false);
		if (rc)
			break;
	}

	of_node_put(root);
	return rc;
}
EXPORT_SYMBOL(of_platform_bus_probe);

/**
 * of_platform_populate() - Populate platform_devices from device tree data
 * @root: parent of the first level to probe or NULL for the root of the tree
 * @matches: match table, NULL to use the default
 * @lookup: auxdata table for matching id and platform_data with device nodes
 * @parent: parent to hook devices from, NULL for toplevel
 *
 * Similar to of_platform_bus_probe(), this function walks the device tree
 * and creates devices from nodes.  It differs in that it follows the modern
 * convention of requiring all device nodes to have a 'compatible' property,
 * and it is suitable for creating devices which are children of the root
 * node (of_platform_bus_probe will only create children of the root which
 * are selected by the @matches argument).
 *
 * New board support should be using this function instead of
 * of_platform_bus_probe().
 *
 * Returns 0 on success, < 0 on failure.
 */
int of_platform_populate(struct device_node *root,
			const struct of_device_id *matches,
			const struct of_dev_auxdata *lookup,
			struct device *parent)
{
	struct device_node *child;
	int rc = 0;

	root = root ? of_node_get(root) : of_find_node_by_path("/");
	if (!root)
		return -EINVAL;

	for_each_child_of_node(root, child) {
		rc = of_platform_bus_create(child, matches, lookup, parent, true);
		if (rc)
			break;
	}

	of_node_put(root);
	return rc;
}
EXPORT_SYMBOL_GPL(of_platform_populate);

/**
 * of_dma_get_range - Get DMA range info
 * @np:		device node to get DMA range info
 * @dma_addr:	pointer to store initial DMA address of DMA range
 * @paddr:	pointer to store initial CPU address of DMA range
 * @size:	pointer to store size of DMA range
 *
 * Look in bottom up direction for the first "dma-range" property
 * and parse it.
 *  dma-ranges format:
 *	DMA addr (dma_addr)	: naddr cells
 *	CPU addr (phys_addr_t)	: pna cells
 *	size			: nsize cells
 *
 * It returns -ENODEV if "dma-ranges" property was not found
 * for this device in DT.
 */
extern int of_dma_get_range(struct device_node *np, dma_addr_t *dma_addr,
		phys_addr_t *paddr, phys_addr_t *size)
{
	struct device_node *node = np;
	const u32 *ranges = NULL;
	int len, naddr, nsize, pna;
	int ret = 0;

	if (!node)
		return -EINVAL;

	while (1) {
		naddr = of_n_addr_cells(node);
		nsize = of_n_size_cells(node);
		node = of_get_next_parent(node);
		if (!node)
			break;

		ranges = of_get_property(node, "dma-ranges", &len);

		/* Ignore empty ranges, they imply no translation required */
		if (ranges && len > 0)
			break;

		/*
		 * At least empty ranges has to be defined for parent node if
		 * DMA is supported
		 */
		if (!ranges)
			break;
	}

	if (!ranges) {
		pr_debug("%s: no dma-ranges found for node(%s)\n",
			 __func__, np->full_name);
		ret = -ENODEV;
		goto out;
	}

	len /= sizeof(u32);

	pna = of_n_addr_cells(node);

	/* dma-ranges format:
	 * DMA addr	: naddr cells
	 * CPU addr	: pna cells
	 * size		: nsize cells
	 */
	*dma_addr = of_read_number(ranges, naddr);
	*paddr = of_translate_dma_address(np, ranges);
	if (*paddr == OF_BAD_ADDR) {
		pr_err("%s: translation of DMA address(%pad) to CPU address failed node(%s)\n",
		       __func__, dma_addr, np->full_name);
		ret = -EINVAL;
	}

	*size = of_read_number(ranges + naddr + pna, nsize);

	pr_debug("dma_addr(%pad) cpu_addr(%pa) size(%pa)\n",
		 dma_addr, paddr, size);

out:
	of_node_put(node);

	return ret;
}
EXPORT_SYMBOL_GPL(of_dma_get_range);

/**
 * of_dma_is_coherent - Check if device is coherent
 * @np:	device node
 *
 * It returns true if "dma-coherent" property was found
 * for this device in DT.
 */
#ifndef CONFIG_ARCH_IS_DMA_COHERENT
bool of_dma_is_coherent(struct device_node *np)
{
	struct device_node *node = np;

	while (node) {
		if (of_property_read_bool(node, "dma-coherent")) {
			of_node_put(node);
			return true;
		}
		node = of_get_next_parent(node);
	}
	return false;
}
EXPORT_SYMBOL_GPL(of_dma_is_coherent);
#else
inline bool of_dma_is_coherent(struct device_node *np)
{
	/* ARCH is fully dma coherent and don't need per device control */
	return true;
}
EXPORT_SYMBOL_GPL(of_dma_is_coherent);
#endif
#endif /* CONFIG_OF_ADDRESS */
