/*
 * Parking Protocol SMP initialisation
 *
 * Based largely on spin-table method.
 *
 * Copyright (C) 2013 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/acpi.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>
#include <asm/system_misc.h>

static phys_addr_t cpu_mailbox_addr[NR_CPUS];

static void (*__smp_boot_wakeup)(int cpu);

void set_smp_boot_wakeup_call(void (*fn)(int cpu))
{
	__smp_boot_wakeup = fn;
}

static int smp_parking_protocol_cpu_init(struct device_node *dn,
					 unsigned int cpu)
{
	/*
	 * Determine the mailbox address.
	 */
	if (!acpi_get_cpu_parked_address(cpu, &cpu_mailbox_addr[cpu])) {
		pr_info("%s: ACPI parked addr=%llx\n",
			__func__, cpu_mailbox_addr[cpu]);
		return 0;
	}

	pr_err("CPU %d: missing or invalid parking protocol mailbox\n", cpu);

	return -1;
}

static int smp_parking_protocol_cpu_prepare(unsigned int cpu)
{
	return 0;
}

struct parking_protocol_mailbox {
	__le32 cpu_id;
	__le32 reserved;
	__le64 entry_point;
};

static int smp_parking_protocol_cpu_boot(unsigned int cpu)
{
	struct parking_protocol_mailbox __iomem *mailbox;
	
	/*begin:add by liufeng*/
	static int count = 0;
	printk("Liufeng: call smp_parking_protocol_cpu_boot %d times\n",count++);
	/*end:add by liufeng*/

	if (!cpu_mailbox_addr[cpu] || !__smp_boot_wakeup)
		return -ENODEV;

	/*
	 * The mailbox may or may not be inside the linear mapping.
	 * As ioremap_cache will either give us a new mapping or reuse the
	 * existing linear mapping, we can use it to cover both cases. In
	 * either case the memory will be MT_NORMAL.
	 */
	mailbox = ioremap_cache(cpu_mailbox_addr[cpu], sizeof(*mailbox));
	if (!mailbox)
		return -ENOMEM;

	/*
	 * We write the entry point and cpu id as LE regardless of the
	 * native endianess of the kernel. Therefore, any boot-loaders
	 * that read this address need to convert this address to the
	 * Boot-Loader's endianess before jumping.
	 */
	writeq(__pa(secondary_entry), &mailbox->entry_point);
	writel(cpu, &mailbox->cpu_id);
	__flush_dcache_area(mailbox, sizeof(*mailbox));
	__smp_boot_wakeup(cpu);

	/* temp hack for broken firmware */
	sev();

	iounmap(mailbox);

	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static int smp_parking_protocol_cpu_disable(unsigned int cpu)
{
	if (!cpu_mailbox_addr[cpu] || !__smp_boot_wakeup) {
		pr_crit("CPU%u will not be disabled\n", cpu);
		return -EOPNOTSUPP;
	}
	return 0;
}
static void smp_parking_protocol_cpu_die(unsigned int cpu)
{
	soft_restart(0);
	pr_crit("unable to power off CPU%u\n", cpu);
}
#endif

const struct cpu_operations smp_parking_protocol_ops = {
	.name		= "parking-protocol",
	.cpu_init	= smp_parking_protocol_cpu_init,
	.cpu_prepare	= smp_parking_protocol_cpu_prepare,
	.cpu_boot	= smp_parking_protocol_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable	= smp_parking_protocol_cpu_disable,
	.cpu_die	= smp_parking_protocol_cpu_die,
#endif
};
