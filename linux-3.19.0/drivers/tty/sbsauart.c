/*
 * SBSA (Server Base System Architecture) Compatible UART driver
 *
 * Copyright (C) 2014 Linaro Ltd
 *
 * Author: Graeme Gregory <graeme.gregory@linaro.org>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/acpi.h>
#include <linux/amba/serial.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

struct sbsa_tty {
	struct tty_port port;
	spinlock_t lock;
	void __iomem *base;
	u32 irq;
	int opencount;
	struct console console;
};

static struct tty_driver *sbsa_tty_driver;
static struct sbsa_tty *sbsa_tty;

#define SBSAUART_CHAR_MASK	0xFF

static void sbsa_tty_do_write(const char *buf, unsigned count)
{
	unsigned long irq_flags;
	struct sbsa_tty *qtty = sbsa_tty;
	void __iomem *base = qtty->base;
	unsigned n;

	spin_lock_irqsave(&qtty->lock, irq_flags);
	for (n = 0; n < count; n++) {
		while (readw(base + UART01x_FR) & UART01x_FR_TXFF)
			mdelay(10);
		writew(buf[n], base + UART01x_DR);
	}
	spin_unlock_irqrestore(&qtty->lock, irq_flags);
}

static void sbsauart_fifo_to_tty(struct sbsa_tty *qtty)
{
	void __iomem *base = qtty->base;
	unsigned int flag, max_count = 32;
	u16 status, ch;

	while (max_count--) {
		status = readw(base + UART01x_FR);
		if (status & UART01x_FR_RXFE)
			break;

		/* Take chars from the FIFO and update status */
		ch = readw(base + UART01x_DR);
		flag = TTY_NORMAL;

		if (ch & UART011_DR_BE)
			flag = TTY_BREAK;
		else if (ch & UART011_DR_PE)
			flag = TTY_PARITY;
		else if (ch & UART011_DR_FE)
			flag = TTY_FRAME;
		else if (ch & UART011_DR_OE)
			flag = TTY_OVERRUN;

		ch &= SBSAUART_CHAR_MASK;

		tty_insert_flip_char(&qtty->port, ch, flag);
	}

	tty_schedule_flip(&qtty->port);

	/* Clear the RX IRQ */
	writew(UART011_RXIC | UART011_RXIC, base + UART011_ICR);
}

static irqreturn_t sbsa_tty_interrupt(int irq, void *dev_id)
{
	struct sbsa_tty *qtty = sbsa_tty;
	unsigned long irq_flags;

	spin_lock_irqsave(&qtty->lock, irq_flags);
	sbsauart_fifo_to_tty(qtty);
	spin_unlock_irqrestore(&qtty->lock, irq_flags);

	return IRQ_HANDLED;
}

static int sbsa_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct sbsa_tty *qtty = sbsa_tty;

	return tty_port_open(&qtty->port, tty, filp);
}

static void sbsa_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static void sbsa_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

static int sbsa_tty_write(struct tty_struct *tty, const unsigned char *buf,
								int count)
{
	sbsa_tty_do_write(buf, count);
	return count;
}

static int sbsa_tty_write_room(struct tty_struct *tty)
{
	return 32;
}

static void sbsa_tty_console_write(struct console *co, const char *b,
								unsigned count)
{
	sbsa_tty_do_write(b, count);
	
	if(b[count - 1] == '\n');
		sbsa_tty_do_write("\r", 1);
}

static struct tty_driver *sbsa_tty_console_device(struct console *c,
								int *index)
{
	*index = c->index;
	return sbsa_tty_driver;
}

static int sbsa_tty_console_setup(struct console *co, char *options)
{
	if ((unsigned)co->index > 0)
		return -ENODEV;
	if (sbsa_tty->base == NULL)
		return -ENODEV;
	return 0;
}

static struct tty_port_operations sbsa_port_ops = {
};

static const struct tty_operations sbsa_tty_ops = {
	.open = sbsa_tty_open,
	.close = sbsa_tty_close,
	.hangup = sbsa_tty_hangup,
	.write = sbsa_tty_write,
	.write_room = sbsa_tty_write_room,
};

static int sbsa_tty_create_driver(void)
{
	int ret;
	struct tty_driver *tty;

	sbsa_tty = kzalloc(sizeof(*sbsa_tty), GFP_KERNEL);
	if (sbsa_tty == NULL) {
		ret = -ENOMEM;
		goto err_alloc_sbsa_tty_failed;
	}
	tty = alloc_tty_driver(1);
	if (tty == NULL) {
		ret = -ENOMEM;
		goto err_alloc_tty_driver_failed;
	}
	tty->driver_name = "sbsauart";
	tty->name = "ttySBSA";
	tty->type = TTY_DRIVER_TYPE_SERIAL;
	tty->subtype = SERIAL_TYPE_NORMAL;
	tty->init_termios = tty_std_termios;
	tty->flags = TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW |
						TTY_DRIVER_DYNAMIC_DEV;
	tty_set_operations(tty, &sbsa_tty_ops);
	ret = tty_register_driver(tty);
	if (ret)
		goto err_tty_register_driver_failed;

	sbsa_tty_driver = tty;
	return 0;

err_tty_register_driver_failed:
	put_tty_driver(tty);
err_alloc_tty_driver_failed:
	kfree(sbsa_tty);
	sbsa_tty = NULL;
err_alloc_sbsa_tty_failed:
	return ret;
}

static void sbsa_tty_delete_driver(void)
{
	tty_unregister_driver(sbsa_tty_driver);
	put_tty_driver(sbsa_tty_driver);
	sbsa_tty_driver = NULL;
	kfree(sbsa_tty);
	sbsa_tty = NULL;
}

static int sbsa_tty_probe(struct platform_device *pdev)
{
	struct sbsa_tty *qtty;
	int ret = -EINVAL;
	int i;
	struct resource *r;
	struct device *ttydev;
	void __iomem *base;
	u32 irq;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL)
		return -EINVAL;

	base = ioremap(r->start, r->end - r->start);
	if (base == NULL)
		pr_err("sbsa_tty: unable to remap base\n");

	r = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (r == NULL)
		goto err_unmap;

	irq = r->start;

	if (pdev->id > 0)
		goto err_unmap;

	ret = sbsa_tty_create_driver();
	if (ret)
		goto err_unmap;

	qtty = sbsa_tty;
	spin_lock_init(&qtty->lock);
	tty_port_init(&qtty->port);
	qtty->port.ops = &sbsa_port_ops;
	qtty->base = base;
	qtty->irq = irq;

	/* Clear and Mask all IRQs */
	writew(0, base + UART011_IMSC);
	writew(0xFFFF, base + UART011_ICR);

	ret = request_irq(irq, sbsa_tty_interrupt, IRQF_SHARED,
						"sbsa_tty", pdev);
	if (ret)
		goto err_request_irq_failed;

	/* Unmask the RX IRQ */
	writew(UART011_RXIM | UART011_RTIM, base + UART011_IMSC);

	ttydev = tty_port_register_device(&qtty->port, sbsa_tty_driver,
							0, &pdev->dev);
	if (IS_ERR(ttydev)) {
		ret = PTR_ERR(ttydev);
		goto err_tty_register_device_failed;
	}

	strcpy(qtty->console.name, "ttySBSA");
	qtty->console.write = sbsa_tty_console_write;
	qtty->console.device = sbsa_tty_console_device;
	qtty->console.setup = sbsa_tty_console_setup;
	qtty->console.flags = CON_PRINTBUFFER;
	/* if no console= on cmdline, make this the console device */
	if (!console_set_on_cmdline)
		qtty->console.flags |= CON_CONSDEV;
	qtty->console.index = pdev->id;
	register_console(&qtty->console);

	return 0;

	tty_unregister_device(sbsa_tty_driver, i);
err_tty_register_device_failed:
	free_irq(irq, pdev);
err_request_irq_failed:
	sbsa_tty_delete_driver();
err_unmap:
	iounmap(base);
	return ret;
}

static int sbsa_tty_remove(struct platform_device *pdev)
{
	struct sbsa_tty *qtty;

	qtty = sbsa_tty;
	unregister_console(&qtty->console);
	tty_unregister_device(sbsa_tty_driver, pdev->id);
	iounmap(qtty->base);
	qtty->base = 0;
	free_irq(qtty->irq, pdev);
	sbsa_tty_delete_driver();
	return 0;
}

static const struct acpi_device_id sbsa_acpi_match[] = {
	{ "ARMH0011", 0 },
	{ }
};

static struct platform_driver sbsa_tty_platform_driver = {
	.probe = sbsa_tty_probe,
	.remove = sbsa_tty_remove,
	.driver = {
		.name = "sbsa_tty",
		.acpi_match_table = ACPI_PTR(sbsa_acpi_match),
	}
};

module_platform_driver(sbsa_tty_platform_driver);

MODULE_LICENSE("GPL v2");
