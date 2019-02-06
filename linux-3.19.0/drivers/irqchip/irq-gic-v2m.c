/*
 * ARM GIC v2m MSI(-X) support
 * Support for Message Signaled Interrupts for systems that
 * implement ARM Generic Interrupt Controller: GICv2m.
 *
 * Copyright (C) 2014 Advanced Micro Devices, Inc.
 * Authors: Suravee Suthikulpanit <suravee.suthikulpanit@amd.com>
 *	    Harish Kasiviswanathan <harish.kasiviswanathan@amd.com>
 *	    Brandon Anderson <brandon.anderson@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#define pr_fmt(fmt) "GICv2m: " fmt

#include <linux/acpi.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/*
* MSI_TYPER:
*     [31:26] Reserved
*     [25:16] lowest SPI assigned to MSI
*     [15:10] Reserved
*     [9:0]   Numer of SPIs assigned to MSI
*/
#define V2M_MSI_TYPER		       0x008
#define V2M_MSI_TYPER_BASE_SHIFT       16
#define V2M_MSI_TYPER_BASE_MASK	       0x3FF
#define V2M_MSI_TYPER_NUM_MASK	       0x3FF
#define V2M_MSI_SETSPI_NS	       0x040
#define V2M_MIN_SPI		       32
#define V2M_MAX_SPI		       1019

#define V2M_MSI_TYPER_BASE_SPI(x)      \
	       (((x) >> V2M_MSI_TYPER_BASE_SHIFT) & V2M_MSI_TYPER_BASE_MASK)

#define V2M_MSI_TYPER_NUM_SPI(x)       ((x) & V2M_MSI_TYPER_NUM_MASK)

struct v2m_data {
	spinlock_t msi_cnt_lock;
	struct resource res;	/* GICv2m resource */
	void __iomem *base;	/* GICv2m virt address */
	u32 spi_start;		/* The SPI number that MSIs start */
	u32 nr_spis;		/* The number of SPIs for MSIs */
	unsigned long *bm;	/* MSI vector bitmap */
	struct irq_domain *domain;
};

static void gicv2m_mask_msi_irq(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void gicv2m_unmask_msi_irq(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip gicv2m_msi_irq_chip = {
	.name			= "MSI",
	.irq_mask		= gicv2m_mask_msi_irq,
	.irq_unmask		= gicv2m_unmask_msi_irq,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_write_msi_msg	= pci_msi_domain_write_msg,
};

static struct msi_domain_info gicv2m_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_PCI_MSIX),
	.chip	= &gicv2m_msi_irq_chip,
};

static int gicv2m_set_affinity(struct irq_data *irq_data,
			       const struct cpumask *mask, bool force)
{
	int ret;

	ret = irq_chip_set_affinity_parent(irq_data, mask, force);
	if (ret == IRQ_SET_MASK_OK)
		ret = IRQ_SET_MASK_OK_DONE;

	return ret;
}

static void gicv2m_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct v2m_data *v2m = irq_data_get_irq_chip_data(data);
	phys_addr_t addr = v2m->res.start + V2M_MSI_SETSPI_NS;

	msg->address_hi = (u32) (addr >> 32);
	msg->address_lo = (u32) (addr);
	msg->data = data->hwirq;
}

static struct irq_chip gicv2m_irq_chip = {
	.name			= "GICv2m",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= gicv2m_set_affinity,
	.irq_compose_msi_msg	= gicv2m_compose_msi_msg,
};

static int gicv2m_irq_gic_domain_alloc(struct irq_domain *domain,
				       unsigned int virq,
				       irq_hw_number_t hwirq)
{
	struct of_phandle_args args;
	struct irq_data *d;
	int err;

	args.np = domain->parent->of_node;
	args.args_count = 3;
	args.args[0] = 0;
	args.args[1] = hwirq - 32;
	args.args[2] = IRQ_TYPE_EDGE_RISING;

	err = irq_domain_alloc_irqs_parent(domain, virq, 1, &args);
	if (err)
		return err;

	/* Configure the interrupt line to be edge */
	d = irq_domain_get_irq_data(domain->parent, virq);
	d->chip->irq_set_type(d, IRQ_TYPE_EDGE_RISING);
	return 0;
}

static void gicv2m_unalloc_msi(struct v2m_data *v2m, unsigned int hwirq)
{
	int pos;

	pos = hwirq - v2m->spi_start;
	if (pos < 0 || pos >= v2m->nr_spis) {
		pr_err("Failed to teardown msi. Invalid hwirq %d\n", hwirq);
		return;
	}

	spin_lock(&v2m->msi_cnt_lock);
	__clear_bit(pos, v2m->bm);
	spin_unlock(&v2m->msi_cnt_lock);
}

static int gicv2m_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				   unsigned int nr_irqs, void *args)
{
	struct v2m_data *v2m = domain->host_data;
	int hwirq, offset, err = 0;

	spin_lock(&v2m->msi_cnt_lock);
	offset = find_first_zero_bit(v2m->bm, v2m->nr_spis);
	if (offset < v2m->nr_spis)
		__set_bit(offset, v2m->bm);
	else
		err = -ENOSPC;
	spin_unlock(&v2m->msi_cnt_lock);

	if (err)
		return err;

	hwirq = v2m->spi_start + offset;

	err = gicv2m_irq_gic_domain_alloc(domain, virq, hwirq);
	if (err) {
		gicv2m_unalloc_msi(v2m, hwirq);
		return err;
	}

	irq_domain_set_hwirq_and_chip(domain, virq, hwirq,
				      &gicv2m_irq_chip, v2m);

	return 0;
}

static void gicv2m_irq_domain_free(struct irq_domain *domain,
				   unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct v2m_data *v2m = irq_data_get_irq_chip_data(d);

	BUG_ON(nr_irqs != 1);
	gicv2m_unalloc_msi(v2m, d->hwirq);
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

static struct irq_domain_ops gicv2m_domain_ops = {
	.alloc			= gicv2m_irq_domain_alloc,
	.free			= gicv2m_irq_domain_free,
};

static bool is_msi_spi_valid(u32 base, u32 num)
{
	if (base < V2M_MIN_SPI) {
		pr_err("Invalid MSI base SPI (base:%u)\n", base);
		return false;
	}

	if ((num == 0) || (base + num > V2M_MAX_SPI)) {
		pr_err("Number of SPIs (%u) exceed maximum (%u)\n",
		       num, V2M_MAX_SPI - V2M_MIN_SPI + 1);
		return false;
	}

	return true;
}

char gicv2m_msi_domain_name[] = "V2M-MSI";
char gicv2m_domain_name[] = "GICV2M";
static int __init gicv2m_init_one(struct irq_domain *parent,
				  u32 spi_start, u32 nr_spis,
				  struct resource *res,
				  struct device_node *node,
				  u32 msi_frame_id)
{
	int ret;
	struct v2m_data *v2m;
	struct irq_domain *inner_domain;

	v2m = kzalloc(sizeof(struct v2m_data), GFP_KERNEL);
	if (!v2m) {
		pr_err("Failed to allocate struct v2m_data.\n");
		return -ENOMEM;
	}

	v2m->base = ioremap(res->start, resource_size(res));
	if (!v2m->base) {
		pr_err("Failed to map GICv2m resource\n");
		ret = -ENOMEM;
		goto err_free_v2m;
	}
	memcpy(&v2m->res,res, sizeof(struct resource));

	if (spi_start && nr_spis) {
		v2m->spi_start = spi_start;
		v2m->nr_spis = nr_spis;
	} else {
		u32 typer = readl_relaxed(v2m->base + V2M_MSI_TYPER);

		v2m->spi_start = V2M_MSI_TYPER_BASE_SPI(typer);
		v2m->nr_spis = V2M_MSI_TYPER_NUM_SPI(typer);
	}

	if (!is_msi_spi_valid(v2m->spi_start, v2m->nr_spis)) {
		ret = -EINVAL;
		goto err_iounmap;
	}

	v2m->bm = kzalloc(sizeof(long) * BITS_TO_LONGS(v2m->nr_spis),
			  GFP_KERNEL);
	if (!v2m->bm) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	inner_domain = irq_domain_add_tree(NULL, &gicv2m_domain_ops, v2m);
	if (!inner_domain) {
		pr_err("Failed to create GICv2m domain\n");
		ret = -ENOMEM;
		goto err_free_bm;
	}

	inner_domain->parent = parent;
	inner_domain->name = gicv2m_domain_name;
	gicv2m_msi_domain_info.acpi_msi_frame_id = msi_frame_id;
	v2m->domain = pci_msi_create_irq_domain(node, &gicv2m_msi_domain_info,
						inner_domain);
	if (!v2m->domain) {
		pr_err("Failed to create MSI domain\n");
		ret = -ENOMEM;
		goto err_free_domains;
	}

	v2m->domain->name = gicv2m_msi_domain_name;

	spin_lock_init(&v2m->msi_cnt_lock);

	return 0;

err_free_domains:
	if (v2m->domain)
		irq_domain_remove(v2m->domain);
	if (inner_domain)
		irq_domain_remove(inner_domain);
err_free_bm:
	kfree(v2m->bm);
err_iounmap:
	iounmap(v2m->base);
err_free_v2m:
	kfree(v2m);
	return ret;
}

static struct of_device_id gicv2m_device_id[] = {
	{	.compatible	= "arm,gic-v2m-frame",	},
	{},
};

int __init gicv2m_of_init(struct device_node *node, struct irq_domain *parent)
{
	int ret = 0;
	struct device_node *child;

	for (child = of_find_matching_node(node, gicv2m_device_id); child;
	     child = of_find_matching_node(child, gicv2m_device_id)) {
		u32 spi_start = 0, nr_spis = 0;
		struct resource res;

		if (!of_find_property(child, "msi-controller", NULL))
			continue;

		ret = of_address_to_resource(child, 0, &res);
		if (ret) {
			pr_err("Failed to allocate v2m resource.\n");
			break;
		}

		if (!of_property_read_u32(child, "arm,msi-base-spi", &spi_start) &&
		    !of_property_read_u32(child, "arm,msi-num-spis", &nr_spis))
			pr_info("Overriding V2M MSI_TYPER (base:%u, num:%u)\n",
				spi_start, nr_spis);

		ret = gicv2m_init_one(parent, spi_start, nr_spis, &res, child, 0);
		if (ret) {
			of_node_put(node);
			break;
		}

		pr_info("Node %s: range[%#lx:%#lx], SPI[%d:%d]\n", child->name,
			(unsigned long)res.start, (unsigned long)res.end,
			spi_start, (spi_start + nr_spis));
	}

	return ret;
}

#ifdef CONFIG_ACPI
static struct acpi_madt_generic_msi_frame *msi_frame;

static int __init
gic_acpi_parse_madt_msi(struct acpi_subtable_header *header,
			const unsigned long end)
{
	struct acpi_madt_generic_msi_frame *frame;

	frame = (struct acpi_madt_generic_msi_frame *)header;
	if (BAD_MADT_ENTRY(frame, end))
		return -EINVAL;

	if (msi_frame)
		pr_warn("Only one GIC MSI FRAME supported.\n");
	else
		msi_frame = frame;

	return 0;
}

int __init gicv2m_acpi_init(struct acpi_table_header *table,
			    struct irq_domain *parent)
{
	int ret = 0;
	int count, i;
	static struct acpi_madt_generic_msi_frame *cur;

	count = acpi_parse_entries(ACPI_SIG_MADT, sizeof(struct acpi_table_madt),
				   gic_acpi_parse_madt_msi, table,
				   ACPI_MADT_TYPE_GENERIC_MSI_FRAME, 0);
	if ((count <= 0) || !msi_frame) {
		pr_debug("No valid ACPI GIC MSI FRAME exist\n");
		return 0;
	}

	for (i = 0, cur = msi_frame; i < count; i++, cur++) {
		struct resource res;
		u32 spi_start = 0, nr_spis = 0;

		res.start = cur->base_address;
		res.end = cur->base_address + 0x1000;

		if (cur->flags & ACPI_MADT_OVERRIDE_SPI_VALUES) {
			spi_start = cur->spi_base;
			nr_spis = cur->spi_count;

			pr_info("ACPI overriding V2M MSI_TYPER (base:%u, num:%u)\n",
				spi_start, nr_spis);
		}

		ret = gicv2m_init_one(parent, spi_start, nr_spis, &res, NULL,
				      cur->msi_frame_id);
		if (ret)
			break;

		pr_info("MSI frame ID %u: range[%#lx:%#lx], SPI[%d:%d]\n",
			cur->msi_frame_id,
			(unsigned long)res.start, (unsigned long)res.end,
			spi_start, (spi_start + nr_spis));
	}
	return ret;
}

#endif /* CONFIG_ACPI */
