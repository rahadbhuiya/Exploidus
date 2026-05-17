#include "serial.h"

#define COM1_PORT  0x3F8

#define SERIAL_DATA       (COM1_PORT + 0)
#define SERIAL_INT_ENABLE (COM1_PORT + 1)
#define SERIAL_FIFO_CTRL  (COM1_PORT + 2)
#define SERIAL_LINE_CTRL  (COM1_PORT + 3)
#define SERIAL_MODEM_CTRL (COM1_PORT + 4)
#define SERIAL_LINE_STAT  (COM1_PORT + 5)

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void)
{
    outb(SERIAL_INT_ENABLE, 0x00);  /* disable interrupts */
    outb(SERIAL_LINE_CTRL,  0x80);  /* enable DLAB */
    outb(SERIAL_DATA,       0x01);  /* baud divisor low  (115200 baud) */
    outb(SERIAL_INT_ENABLE, 0x00);  /* baud divisor high */
    outb(SERIAL_LINE_CTRL,  0x03);  /* 8 bits, no parity, one stop bit */
    outb(SERIAL_FIFO_CTRL,  0xC7);  /* enable FIFO, clear, 14-byte threshold */
    outb(SERIAL_MODEM_CTRL, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

static int serial_transmit_empty(void)
{
    return inb(SERIAL_LINE_STAT) & 0x20;
}

void serial_putc(char c)
{
    while (!serial_transmit_empty());
    outb(SERIAL_DATA, (uint8_t)c);
}

void serial_print(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_printhex(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[17 - i] = hex[val & 0xF], val >>= 4;
    buf[18] = '\0';
    serial_print(buf);
}

bool serial_read_byte(char *out)
{
    /* Line Status Register bit 0 = data ready */
    if (inb(0x3F8 + 5) & 0x01) {
        *out = (char)inb(0x3F8);
        return true;
    }
    return false;
}
