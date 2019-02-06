#ifndef __ASM_PCI_H
#define __ASM_PCI_H
#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <asm/io.h>
#include <asm-generic/pci-bridge.h>
#include <asm-generic/pci-dma-compat.h>

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0

/*
 * Set to 1 if the kernel should re-assign all PCI bus numbers
 */
#define pcibios_assign_all_busses() \
	(pci_has_flag(PCI_REASSIGN_ALL_BUS))

/*
 * PCI address space differs from physical memory address space
 */
#define PCI_DMA_BUS_IS_PHYS	(0)

static inline int pci_get_legacy_ide_irq(struct pci_dev *dev, int channel)
{
	/* no legacy IRQ on arm64 */
	return -ENODEV;
}

extern int isa_dma_bridge_buggy;

#ifdef CONFIG_PCI
static inline int pci_proc_domain(struct pci_bus *bus)
{
	return 1;
}
#endif  /* CONFIG_PCI */

struct acpi_device;

struct pci_sysdata {
	int		domain;		/* PCI domain */
	int		node;		/* NUMA node */
	struct acpi_device *companion;	/* ACPI companion device */
	void		*iommu;		/* IOMMU private data */
};


static inline unsigned char mmio_config_readb(void __iomem *pos)
{
	int offset = (__force unsigned long)pos & 3;
	int shift = offset * 8;

	return readl(pos - offset) >> shift;
}

static inline unsigned short mmio_config_readw(void __iomem *pos)
{
	int offset = (__force unsigned long)pos & 3;
	int shift = offset * 8;

	return readl(pos - offset) >> shift;
}

static inline unsigned int mmio_config_readl(void __iomem *pos)
{
	return readl(pos);
}

static inline void mmio_config_writeb(void __iomem *pos, u8 val)
{
	int offset = (__force unsigned long)pos & 3;
	int shift = offset * 8;
	int mask = ~(0xff << shift);
	u32 v;

	pos -= offset;
	v = readl(pos) & mask;
	writel(v | (val << shift), pos);
}

static inline void mmio_config_writew(void __iomem *pos, u16 val)
{
	int offset = (__force unsigned long)pos & 3;
	int shift = offset * 8;
	int mask = ~(0xffff << shift);
	u32 v;

	pos -= offset;
	v = readl(pos) & mask;
	writel(v | (val << shift), pos);
}

static inline void mmio_config_writel(void __iomem *pos, u32 val)
{
	writel(val, pos);
}

#endif  /* __KERNEL__ */
#endif  /* __ASM_PCI_H */
