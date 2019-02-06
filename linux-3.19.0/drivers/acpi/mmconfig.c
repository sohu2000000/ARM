/*
 * Arch agnostic low-level direct PCI config space access via MMCONFIG
 *
 * Per-architecture code takes care of the mappings, region validation and
 * accesses themselves.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/pci.h>
#include <linux/mmconfig.h>

#include <asm/pci.h>

#define PREFIX "PCI: "

static DEFINE_MUTEX(pci_mmcfg_lock);

LIST_HEAD(pci_mmcfg_list);

extern struct acpi_mcfg_fixup __start_acpi_mcfg_fixups[];
extern struct acpi_mcfg_fixup __end_acpi_mcfg_fixups[];

/*
 * raw_pci_read/write - ACPI PCI config space accessors.
 *
 * ACPI spec defines MMCFG as the way we can access PCI config space,
 * so let MMCFG be default (__weak).
 *
 * If platform needs more fancy stuff, should provides its own implementation.
 */
int __weak raw_pci_read(unsigned int domain, unsigned int bus,
			unsigned int devfn, int reg, int len, u32 *val)
{
	return pci_mmcfg_read(domain, bus, devfn, reg, len, val);
}

int __weak raw_pci_write(unsigned int domain, unsigned int bus,
			 unsigned int devfn, int reg, int len, u32 val)
{
	return pci_mmcfg_write(domain, bus, devfn, reg, len, val);
}

int __weak pci_mmcfg_read(unsigned int seg, unsigned int bus,
			  unsigned int devfn, int reg, int len, u32 *value)
{
	struct pci_mmcfg_region *cfg;
	char __iomem *addr;

	/* Why do we have this when nobody checks it. How about a BUG()!? -AK */
	if (unlikely((bus > 255) || (devfn > 255) || (reg > 4095))) {
err:		*value = -1;
		return -EINVAL;
	}

	rcu_read_lock();
	cfg = pci_mmconfig_lookup(seg, bus);
	if (!cfg || !cfg->virt) {
		rcu_read_unlock();
		goto err;
	}
	if (cfg->read)
		(*cfg->read)(cfg, bus, devfn, reg, len, value);
	else {
		addr = cfg->virt + (PCI_MMCFG_BUS_OFFSET(bus) | (devfn << 12));

		switch (len) {
		case 1:
			*value = mmio_config_readb(addr + reg);
			break;
		case 2:
			*value = mmio_config_readw(addr + reg);
			break;
		case 4:
			*value = mmio_config_readl(addr + reg);
			break;
		}
	}
	rcu_read_unlock();

	return 0;
}

int __weak pci_mmcfg_write(unsigned int seg, unsigned int bus,
			   unsigned int devfn, int reg, int len, u32 value)
{
	struct pci_mmcfg_region *cfg;
	char __iomem *addr;

	/* Why do we have this when nobody checks it. How about a BUG()!? -AK */
	if (unlikely((bus > 255) || (devfn > 255) || (reg > 4095)))
		return -EINVAL;

	rcu_read_lock();
	cfg = pci_mmconfig_lookup(seg, bus);
	if (!cfg || !cfg->virt) {
		rcu_read_unlock();
		return -EINVAL;
	}
	if (cfg->write)
		(*cfg->write)(cfg, bus, devfn, reg, len, value);
	else {
		addr = cfg->virt + (PCI_MMCFG_BUS_OFFSET(bus) | (devfn << 12));

		switch (len) {
		case 1:
			mmio_config_writeb(addr + reg, value);
			break;
		case 2:
			mmio_config_writew(addr + reg, value);
			break;
		case 4:
			mmio_config_writel(addr + reg, value);
			break;
		}
	}
	rcu_read_unlock();

	return 0;
}

static void __iomem *mcfg_ioremap(struct pci_mmcfg_region *cfg)
{
	void __iomem *addr;
	u64 start, size;
	int num_buses;

	start = cfg->address + PCI_MMCFG_BUS_OFFSET(cfg->start_bus);
	num_buses = cfg->end_bus - cfg->start_bus + 1;
	size = PCI_MMCFG_BUS_OFFSET(num_buses);
	addr = ioremap_nocache(start, size);
	if (addr)
		addr -= PCI_MMCFG_BUS_OFFSET(cfg->start_bus);
	return addr;
}

int __init __weak pci_mmcfg_arch_init(void)
{
	struct pci_mmcfg_region *cfg;

	list_for_each_entry(cfg, &pci_mmcfg_list, list)
		if (pci_mmcfg_arch_map(cfg)) {
			pci_mmcfg_arch_free();
			return 0;
		}

	return 1;
}

void __init __weak pci_mmcfg_arch_free(void)
{
	struct pci_mmcfg_region *cfg;

	list_for_each_entry(cfg, &pci_mmcfg_list, list)
		pci_mmcfg_arch_unmap(cfg);
}

int __weak pci_mmcfg_arch_map(struct pci_mmcfg_region *cfg)
{
	cfg->virt = mcfg_ioremap(cfg);
	if (!cfg->virt) {
		pr_err(PREFIX "can't map MMCONFIG at %pR\n", &cfg->res);
		return -ENOMEM;
	}

	return 0;
}

void __weak pci_mmcfg_arch_unmap(struct pci_mmcfg_region *cfg)
{
	if (cfg && cfg->virt) {
		iounmap(cfg->virt + PCI_MMCFG_BUS_OFFSET(cfg->start_bus));
		cfg->virt = NULL;
	}
}

static void __init pci_mmconfig_remove(struct pci_mmcfg_region *cfg)
{
	if (cfg->res.parent)
		release_resource(&cfg->res);
	list_del(&cfg->list);
	kfree(cfg);
}

void __init free_all_mmcfg(void)
{
	struct pci_mmcfg_region *cfg, *tmp;

	pci_mmcfg_arch_free();
	list_for_each_entry_safe(cfg, tmp, &pci_mmcfg_list, list)
		pci_mmconfig_remove(cfg);
}

void list_add_sorted(struct pci_mmcfg_region *new)
{
	struct pci_mmcfg_region *cfg;

	/* keep list sorted by segment and starting bus number */
	list_for_each_entry_rcu(cfg, &pci_mmcfg_list, list) {
		if (cfg->segment > new->segment ||
		    (cfg->segment == new->segment &&
		     cfg->start_bus >= new->start_bus)) {
			list_add_tail_rcu(&new->list, &cfg->list);
			return;
		}
	}
	list_add_tail_rcu(&new->list, &pci_mmcfg_list);
}

struct pci_mmcfg_region *pci_mmconfig_alloc(int segment, int start,
					    int end, u64 addr)
{
	struct pci_mmcfg_region *new;
	struct resource *res;

	if (addr == 0)
		return NULL;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;

	new->address = addr;
	new->segment = segment;
	new->start_bus = start;
	new->end_bus = end;

	res = &new->res;
	res->start = addr + PCI_MMCFG_BUS_OFFSET(start);
	res->end = addr + PCI_MMCFG_BUS_OFFSET(end + 1) - 1;
	res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;
	snprintf(new->name, PCI_MMCFG_RESOURCE_NAME_LEN,
		 "PCI MMCONFIG %04x [bus %02x-%02x]", segment, start, end);
	res->name = new->name;

	return new;
}

struct pci_mmcfg_region *pci_mmconfig_add(int segment, int start,
					  int end, u64 addr)
{
	struct pci_mmcfg_region *new;

	new = pci_mmconfig_alloc(segment, start, end, addr);
	if (new) {
		mutex_lock(&pci_mmcfg_lock);
		list_add_sorted(new);
		mutex_unlock(&pci_mmcfg_lock);

		pr_info(PREFIX
		       "MMCONFIG for domain %04x [bus %02x-%02x] at %pR "
		       "(base %#lx)\n",
		       segment, start, end, &new->res, (unsigned long)addr);
	}

	return new;
}

int __init pci_mmconfig_inject(struct pci_mmcfg_region *cfg)
{
	struct pci_mmcfg_region *cfg_conflict;
	int err = 0;

	mutex_lock(&pci_mmcfg_lock);
	cfg_conflict = pci_mmconfig_lookup(cfg->segment, cfg->start_bus);
	if (cfg_conflict) {
		if (cfg_conflict->end_bus < cfg->end_bus)
			pr_info(FW_INFO "MMCONFIG for "
				"domain %04x [bus %02x-%02x] "
				"only partially covers this bridge\n",
				cfg_conflict->segment, cfg_conflict->start_bus,
				cfg_conflict->end_bus);
		err = -EEXIST;
		goto out;
	}

	if (pci_mmcfg_arch_map(cfg)) {
		pr_warn("fail to map MMCONFIG %pR.\n", &cfg->res);
		err = -ENOMEM;
		goto out;
	} else {
		list_add_sorted(cfg);
		pr_info("MMCONFIG at %pR (base %#lx)\n",
			&cfg->res, (unsigned long)cfg->address);

	}
out:
	mutex_unlock(&pci_mmcfg_lock);
	return err;
}

struct pci_mmcfg_region *pci_mmconfig_lookup(int segment, int bus)
{
	struct pci_mmcfg_region *cfg;

	list_for_each_entry_rcu(cfg, &pci_mmcfg_list, list)
		if (cfg->segment == segment &&
		    cfg->start_bus <= bus && bus <= cfg->end_bus)
			return cfg;

	return NULL;
}

int __init __weak acpi_mcfg_check_entry(struct acpi_table_mcfg *mcfg,
					struct acpi_mcfg_allocation *cfg)
{
	return 0;
}

int __init pci_parse_mcfg(struct acpi_table_header *header)
{
	struct acpi_table_mcfg *mcfg;
	struct acpi_mcfg_allocation *cfg_table, *cfg;
	struct acpi_mcfg_fixup *fixup;
	struct pci_mmcfg_region *new;
	unsigned long i;
	int entries;

	if (!header)
		return -EINVAL;

	mcfg = (struct acpi_table_mcfg *)header;

	/* how many config structures do we have */
	free_all_mmcfg();
	entries = 0;
	i = header->length - sizeof(struct acpi_table_mcfg);
	while (i >= sizeof(struct acpi_mcfg_allocation)) {
		entries++;
		i -= sizeof(struct acpi_mcfg_allocation);
	}
	if (entries == 0) {
		pr_err(PREFIX "MMCONFIG has no entries\n");
		return -ENODEV;
	}

	fixup = __start_acpi_mcfg_fixups;
	while (fixup < __end_acpi_mcfg_fixups) {
		if (!strncmp(fixup->oem_id, header->oem_id, 6) &&
		    !strncmp(fixup->oem_table_id, header->oem_table_id, 8))
			break;
		++fixup;
	}

	cfg_table = (struct acpi_mcfg_allocation *) &mcfg[1];
	for (i = 0; i < entries; i++) {
		cfg = &cfg_table[i];
		if (acpi_mcfg_check_entry(mcfg, cfg)) {
			free_all_mmcfg();
			return -ENODEV;
		}

		new = pci_mmconfig_add(cfg->pci_segment, cfg->start_bus_number,
				       cfg->end_bus_number, cfg->address);
		if (!new) {
			pr_warn(PREFIX "no memory for MCFG entries\n");
			free_all_mmcfg();
			return -ENOMEM;
		}
		if (fixup < __end_acpi_mcfg_fixups)
			new->fixup = fixup->hook;
	}

	return 0;
}

/* Delete MMCFG information for host bridges */
int pci_mmconfig_delete(u16 seg, u8 start, u8 end)
{
	struct pci_mmcfg_region *cfg;

	mutex_lock(&pci_mmcfg_lock);
	list_for_each_entry_rcu(cfg, &pci_mmcfg_list, list)
		if (cfg->segment == seg && cfg->start_bus == start &&
		    cfg->end_bus == end) {
			list_del_rcu(&cfg->list);
			synchronize_rcu();
			pci_mmcfg_arch_unmap(cfg);
			if (cfg->res.parent)
				release_resource(&cfg->res);
			mutex_unlock(&pci_mmcfg_lock);
			kfree(cfg);
			return 0;
		}
	mutex_unlock(&pci_mmcfg_lock);

	return -ENOENT;
}

void __init __weak pci_mmcfg_early_init(void)
{

}

void __init __weak pci_mmcfg_late_init(void)
{
	struct pci_mmcfg_region *cfg;

	acpi_table_parse(ACPI_SIG_MCFG, pci_parse_mcfg);

	if (list_empty(&pci_mmcfg_list))
		return;

	if (!pci_mmcfg_arch_init())
		free_all_mmcfg();

	list_for_each_entry(cfg, &pci_mmcfg_list, list)
		insert_resource(&iomem_resource, &cfg->res);
}
