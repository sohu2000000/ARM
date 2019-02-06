/*
 * Code borrowed from powerpc/kernel/pci-common.c
 *
 * Copyright (C) 2003 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/of_pci.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include <linux/mmconfig.h>

#include <asm/pci-bridge.h>

/*
 * Called after each bus is probed, but before its children are examined
 */
void pcibios_fixup_bus(struct pci_bus *bus)
{
	/* nothing to do, expected to be removed in the future */
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here
 */
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				resource_size_t size, resource_size_t align)
{
	return res->start;
}

int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	struct pci_sysdata *sd;

	if (!acpi_disabled) {
		sd = bridge->bus->sysdata;
		ACPI_COMPANION_SET(&bridge->dev, sd->companion);
	}
	return 0;
}

/*
 * Try to assign the IRQ number from DT when adding a new device
 */
int pcibios_add_device(struct pci_dev *dev)
{
	if (acpi_disabled)
		dev->irq = of_irq_parse_and_map_pci(dev, 0, 0);

	return 0;
}

void pcibios_add_bus(struct pci_bus *bus)
{
	if (!acpi_disabled)
		acpi_pci_add_bus(bus);
}

void pcibios_remove_bus(struct pci_bus *bus)
{
	if (!acpi_disabled)
		acpi_pci_remove_bus(bus);
}

int pcibios_enable_irq(struct pci_dev *dev)
{
	if (!acpi_disabled && !pci_dev_msi_enabled(dev))
		acpi_pci_irq_enable(dev);
	return 0;
}

int pcibios_disable_irq(struct pci_dev *dev)
{
	if (!acpi_disabled && !pci_dev_msi_enabled(dev))
		acpi_pci_irq_disable(dev);
	return 0;
}

int pcibios_enable_device(struct pci_dev *dev, int bars)
{
	int err;

	err = pci_enable_resources(dev, bars);
	if (err < 0)
		return err;

	if (!pci_dev_msi_enabled(dev))
		return pcibios_enable_irq(dev);
	return 0;
}

#ifdef CONFIG_PCI_DOMAINS_GENERIC
static bool dt_domain_found = false;
void pci_bus_assign_domain_nr(struct pci_bus *bus, struct device *parent)
{
	int domain = -1;

	if (acpi_disabled) {
		domain = of_get_pci_domain_nr(parent->of_node);

		if (domain >= 0) {
			dt_domain_found = true;
		} else if (dt_domain_found == true) {
			dev_err(parent, "Node %s is missing \"linux,pci-domain\" property in DT\n",
				parent->of_node->full_name);
			return;
		} else {
			domain = pci_get_new_domain_nr();
		}
	} else {
		struct pci_sysdata *sd = bus->sysdata;

		domain = sd->domain;
	}
	if (domain >= 0)
		bus->domain_nr = domain;
}
#endif

static int __init pcibios_assign_resources(void)
{
	struct pci_bus *root_bus;

	if (acpi_disabled)
		return 0;

	list_for_each_entry(root_bus, &pci_root_buses, node) {
		pcibios_resource_survey_bus(root_bus);
		pci_assign_unassigned_root_bus_resources(root_bus);
	}
	return 0;
}
/*
 * fs_initcall comes after subsys_initcall, so we know acpi scan
 * has run.
 */
fs_initcall(pcibios_assign_resources);

#ifdef CONFIG_ACPI

static int pci_read(struct pci_bus *bus, unsigned int devfn, int where,
		    int size, u32 *value)
{
	return raw_pci_read(pci_domain_nr(bus), bus->number,
			    devfn, where, size, value);
}

static int pci_write(struct pci_bus *bus, unsigned int devfn, int where,
		     int size, u32 value)
{
	return raw_pci_write(pci_domain_nr(bus), bus->number,
			     devfn, where, size, value);
}

struct pci_ops pci_root_ops = {
	.read = pci_read,
	.write = pci_write,
};

struct pci_root_info {
	struct acpi_device *bridge;
	char name[16];
	unsigned int res_num;
	struct resource *res;
	resource_size_t *res_offset;
	struct pci_sysdata sd;
	u16 segment;
	u8 start_bus;
	u8 end_bus;
};

static acpi_status resource_to_addr(struct acpi_resource *resource,
				    struct acpi_resource_address64 *addr)
{
	acpi_status status;

	memset(addr, 0, sizeof(*addr));
	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS16:
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		status = acpi_resource_to_address64(resource, addr);
		if (ACPI_SUCCESS(status) &&
		    (addr->resource_type == ACPI_MEMORY_RANGE ||
		    addr->resource_type == ACPI_IO_RANGE) &&
		    addr->address_length > 0) {
			return AE_OK;
		}
		break;
	}
	return AE_ERROR;
}

static acpi_status count_resource(struct acpi_resource *acpi_res, void *data)
{
	struct pci_root_info *info = data;
	struct acpi_resource_address64 addr;
	acpi_status status;

	status = resource_to_addr(acpi_res, &addr);
	if (ACPI_SUCCESS(status))
		info->res_num++;
	return AE_OK;
}

static acpi_status setup_resource(struct acpi_resource *acpi_res, void *data)
{
	struct pci_root_info *info = data;
	struct resource *res;
	struct acpi_resource_address64 addr;
	acpi_status status;
	unsigned long flags;
	u64 start, end;

	status = resource_to_addr(acpi_res, &addr);
	if (!ACPI_SUCCESS(status))
		return AE_OK;

	if (addr.resource_type == ACPI_MEMORY_RANGE) {
		flags = IORESOURCE_MEM;
		if (addr.info.mem.caching == ACPI_PREFETCHABLE_MEMORY)
			flags |= IORESOURCE_PREFETCH;
	} else if (addr.resource_type == ACPI_IO_RANGE) {
		flags = IORESOURCE_IO;
	} else
		return AE_OK;

	start = addr.minimum + addr.translation_offset;
	end = addr.maximum + addr.translation_offset;

	res = &info->res[info->res_num];
	res->name = info->name;
	res->flags = flags;
	res->start = start;
	res->end = end;

	if (flags & IORESOURCE_IO) {
		unsigned long port;
		int err;

		err = pci_register_io_range(start, addr.address_length);
		if (err)
			return AE_OK;

		port = pci_address_to_pio(start);
		if (port == (unsigned long)-1) {
			res->start = -1;
			res->end = -1;
			return AE_OK;
		}

		res->start = port;
		res->end = res->start + addr.address_length - 1;

		if (pci_remap_iospace(res, start) < 0)
			return AE_OK;

		info->res_offset[info->res_num] = port - addr.minimum;
	} else
		info->res_offset[info->res_num] = addr.translation_offset;

	info->res_num++;

	return AE_OK;
}

static void coalesce_windows(struct pci_root_info *info, unsigned long type)
{
	int i, j;
	struct resource *res1, *res2;

	for (i = 0; i < info->res_num; i++) {
		res1 = &info->res[i];
		if (!(res1->flags & type))
			continue;

		for (j = i + 1; j < info->res_num; j++) {
			res2 = &info->res[j];
			if (!(res2->flags & type))
				continue;

			/*
			 * I don't like throwing away windows because then
			 * our resources no longer match the ACPI _CRS, but
			 * the kernel resource tree doesn't allow overlaps.
			 */
			if (resource_overlaps(res1, res2)) {
				res2->start = min(res1->start, res2->start);
				res2->end = max(res1->end, res2->end);
				dev_info(&info->bridge->dev,
					 "host bridge window expanded to %pR; %pR ignored\n",
					 res2, res1);
				res1->flags = 0;
			}
		}
	}
}

static void add_resources(struct pci_root_info *info,
			  struct list_head *resources)
{
	int i;
	struct resource *res, *root, *conflict;

	coalesce_windows(info, IORESOURCE_MEM);
	coalesce_windows(info, IORESOURCE_IO);

	for (i = 0; i < info->res_num; i++) {
		res = &info->res[i];

		if (res->flags & IORESOURCE_MEM)
			root = &iomem_resource;
		else if (res->flags & IORESOURCE_IO)
			root = &ioport_resource;
		else
			continue;

		conflict = insert_resource_conflict(root, res);
		if (conflict)
			dev_info(&info->bridge->dev,
				 "ignoring host bridge window %pR (conflicts with %s %pR)\n",
				 res, conflict->name, conflict);
		else
			pci_add_resource_offset(resources, res,
						info->res_offset[i]);
	}
}

static void free_pci_root_info_res(struct pci_root_info *info)
{
	kfree(info->res);
	info->res = NULL;
	kfree(info->res_offset);
	info->res_offset = NULL;
	info->res_num = 0;
}

static void __release_pci_root_info(struct pci_root_info *info)
{
	int i;
	struct resource *res;

	for (i = 0; i < info->res_num; i++) {
		res = &info->res[i];

		if (!res->parent)
			continue;

		if (!(res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
			continue;

		release_resource(res);
	}

	free_pci_root_info_res(info);

	kfree(info);
}

static void release_pci_root_info(struct pci_host_bridge *bridge)
{
	struct pci_root_info *info = bridge->release_data;

	__release_pci_root_info(info);
}

static void probe_pci_root_info(struct pci_root_info *info,
				struct acpi_device *device,
				int busnum, int domain)
{
	size_t size;

	sprintf(info->name, "PCI Bus %04x:%02x", domain, busnum);
	info->bridge = device;

	info->res_num = 0;
	acpi_walk_resources(device->handle, METHOD_NAME__CRS, count_resource,
				info);
	if (!info->res_num)
		return;

	size = sizeof(*info->res) * info->res_num;
	info->res = kzalloc_node(size, GFP_KERNEL, info->sd.node);
	if (!info->res) {
		info->res_num = 0;
		return;
	}

	size = sizeof(*info->res_offset) * info->res_num;
	info->res_num = 0;
	info->res_offset = kzalloc_node(size, GFP_KERNEL, info->sd.node);
	if (!info->res_offset) {
		kfree(info->res);
		info->res = NULL;
		return;
	}

	acpi_walk_resources(device->handle, METHOD_NAME__CRS, setup_resource,
				info);
}

/* Root bridge scanning */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_device *device = root->device;
	struct pci_mmcfg_region *mcfg;
	struct pci_root_info *info;
	int domain = root->segment;
	int busnum = root->secondary.start;
	LIST_HEAD(resources);
	struct pci_bus *bus;
	struct pci_sysdata *sd;
	int node;

	/* we need mmconfig */
	mcfg = pci_mmconfig_lookup(domain, busnum);
	if (!mcfg) {
		pr_err("pci_bus %04x:%02x has no MCFG table\n",
		       domain, busnum);
		return NULL;
	}

	/* temporary hack */
	if (mcfg->fixup)
		(*mcfg->fixup)(root, mcfg);

	if (domain && !pci_domains_supported) {
		pr_warn("PCI %04x:%02x: multiple domains not supported.\n",
			domain, busnum);
		return NULL;
	}

	node = NUMA_NO_NODE;

	info = kzalloc_node(sizeof(*info), GFP_KERNEL, node);
	if (!info) {
		pr_warn("PCI %04x:%02x: ignored (out of memory)\n",
			domain, busnum);
		return NULL;
	}
	info->segment = domain;
	info->start_bus = busnum;
	info->end_bus = root->secondary.end;

	sd = &info->sd;
	sd->domain = domain;
	sd->node = node;
	sd->companion = device;

	probe_pci_root_info(info, device, busnum, domain);

	/* insert busn res at first */
	pci_add_resource(&resources,  &root->secondary);

	/* then _CRS resources */
	add_resources(info, &resources);

	bus = pci_create_root_bus(NULL, busnum, &pci_root_ops, sd, &resources);
	if (bus) {
		pci_scan_child_bus(bus);
		pci_set_host_bridge_release(to_pci_host_bridge(bus->bridge),
					    release_pci_root_info, info);
	} else {
		pci_free_resource_list(&resources);
		__release_pci_root_info(info);
	}

	/* After the PCI-E bus has been walked and all devices discovered,
	 * configure any settings of the fabric that might be necessary.
	 */
	if (bus) {
		struct pci_bus *child;

		list_for_each_entry(child, &bus->children, node)
			pcie_bus_configure_settings(child);
	}

	if (bus && node != NUMA_NO_NODE)
		dev_printk(KERN_DEBUG, &bus->dev, "on NUMA node %d\n", node);

	return bus;
}
#endif
