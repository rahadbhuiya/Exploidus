#include "irq.h"
#include "idt.h"
#include <stdint.h>
#include <string.h>
#include "../../drivers/serial.h"

/* PIC ports */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20
#define PIC_INIT   0x11

#define IRQ_BASE   32
#define IRQ_COUNT  16

static irq_handler_fn g_handlers[IRQ_COUNT];

/* IO */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/* disable all IRQs first (IMPORTANT for stability) */
static void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* enable only safe IRQs (timer + keyboard) */
static void pic_enable_minimal(void)
{
    // IRQ0 = timer, IRQ1 = keyboard
    outb(PIC1_DATA, 0xFC); // 11111100
    outb(PIC2_DATA, 0xFF);
}

static void pic_remap(void)
{
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, PIC_INIT);
    outb(PIC2_CMD, PIC_INIT);

    outb(PIC1_DATA, 32);   // IRQ0-7 -> 32-39
    outb(PIC2_DATA, 40);   // IRQ8-15 -> 40-47

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void irq_init(void)
{
    memset(g_handlers, 0, sizeof(g_handlers));

    pic_remap();

    /* CRITICAL FIX */
    pic_mask_all();
    pic_enable_minimal();
}

void irq_register(uint8_t irq, irq_handler_fn fn)
{
    if (irq >= IRQ_COUNT) return;
    g_handlers[irq] = fn;

    /* unmask this IRQ in PIC */
    if (irq < 8) {
        uint8_t mask = inb(PIC1_DATA);
        mask &= ~(1 << irq);
        outb(PIC1_DATA, mask);
    } else {
        /* slave PIC এর জন্য IRQ2 (cascade) ও unmask করতে হবে */
        uint8_t mask = inb(PIC2_DATA);
        mask &= ~(1 << (irq - 8));
        outb(PIC2_DATA, mask);

        /* master PIC এ IRQ2 cascade line unmask */
        uint8_t master = inb(PIC1_DATA);
        master &= ~(1 << 2);
        outb(PIC1_DATA, master);
    }
}

void irq_dispatch(interrupt_frame_t *frame)
{
    uint8_t vec = frame->int_num;

    /* validate IRQ range */
    if (vec < IRQ_BASE || vec >= IRQ_BASE + IRQ_COUNT)
        return;

    uint8_t irq = vec - IRQ_BASE;

    /* Poll network on every timer tick */
    if (irq == 0) {
        extern void net_poll(void);
        net_poll();
    }

    /* call handler if exists */
    if (g_handlers[irq])
        g_handlers[irq](frame);

    irq_eoi(irq);
}

void irq_eoi(uint8_t irq)
{
    /* spurious IRQ filter */
    if (irq >= IRQ_COUNT)
        return;

    /* slave PIC */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);

    /* master PIC */
    outb(PIC1_CMD, PIC_EOI);
}