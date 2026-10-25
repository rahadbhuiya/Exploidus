#include "../arch/x86_64/irq.h"
#include "../drivers/serial.h"
#include <stdint.h>
#include <stdbool.h>

#define KB_DATA_PORT   0x60
#define KB_STATUS_PORT 0x64
#define KB_IRQ         1
#define KB_RING_SIZE   256

static uint8_t  g_ring[KB_RING_SIZE];
static uint32_t g_ring_head = 0;
static uint32_t g_ring_tail = 0;
static bool     g_shift     = false;
static bool     g_caps      = false;
static bool     g_e0        = false;   /* extended scancode prefix */

static const char g_scancode_normal[128] = {
    0, 0x1B,
    '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\',
    'z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ', 0,
};
static const char g_scancode_shift[128] = {
    0, 0x1B,
    '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|',
    'Z','X','C','V','B','N','M','<','>','?',
    0, '*', 0, ' ', 0,
};

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Push one byte into ring — returns false if full */
static bool ring_push(uint8_t b)
{
    uint32_t next = (g_ring_head + 1) & (KB_RING_SIZE - 1);
    if (next == g_ring_tail) return false;   /* full */
    g_ring[g_ring_head] = b;
    g_ring_head = next;
    return true;
}

/* Push a 3-byte ANSI escape: ESC [ c */
static void push_ansi(char c)
{
    ring_push(0x1B);
    ring_push('[');
    ring_push((uint8_t)c);
}

static void keyboard_irq_handler(interrupt_frame_t *frame)
{
    (void)frame;

    uint8_t status = inb(KB_STATUS_PORT);
    if (!(status & 0x01)) return; /* output buffer empty */
    if (status & 0x20) return;   /* mouse data, leave for mouse handler */

    uint8_t scancode = inb(KB_DATA_PORT);

    /* Extended scancode prefix E0 — next byte is the real scancode */
    if (scancode == 0xE0) { g_e0 = true; return; }

    /* Key release: bit 7 set */
    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;
        if (key == 0x2A || key == 0x36) g_shift = false;
        g_e0 = false;
        return;
    }

    /* ── Extended (E0) scancodes — arrow keys, etc. ── */
    if (g_e0) {
        g_e0 = false;
        switch (scancode) {
            case 0x48: push_ansi('A'); return;   /* UP    -> ESC [ A */
            case 0x50: push_ansi('B'); return;   /* DOWN  -> ESC [ B */
            case 0x4D: push_ansi('C'); return;   /* RIGHT -> ESC [ C */
            case 0x4B: push_ansi('D'); return;   /* LEFT  -> ESC [ D */
            case 0x47: push_ansi('H'); return;   /* Home  -> ESC [ H */
            case 0x4F: push_ansi('F'); return;   /* End   -> ESC [ F */
            /* Delete: ESC [ 3 ~ (4 bytes) */
            case 0x53:
                ring_push(0x1B); ring_push('[');
                ring_push('3');  ring_push('~');
                return;
            default: return;   /* ignore other extended keys */
        }
    }

    /* ── Normal scancodes ── */
    if (scancode == 0x2A || scancode == 0x36) { g_shift = true;  return; }
    if (scancode == 0x3A)                      { g_caps  = !g_caps; return; }
    if (scancode >= 128)                        return;

    char c;
    if (g_shift) {
        c = g_scancode_shift[scancode];
    } else {
        c = g_scancode_normal[scancode];
        if (g_caps && c >= 'a' && c <= 'z')
            c = (char)(c - 32);
    }
    if (!c) return;

    ring_push((uint8_t)c);
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static void kb_wait_write(void)
{
    uint32_t t = 100000;
    while (t-- && (inb(KB_STATUS_PORT) & 2));
}

static void kb_wait_read(void)
{
    uint32_t t = 100000;
    while (t-- && !(inb(KB_STATUS_PORT) & 1));
}

void keyboard_init(void)
{
    serial_print("[KBD] initializing PS/2 keyboard\n");

    /* Flush leftover bytes */
    uint32_t flush = 100;
    while (flush-- && (inb(KB_STATUS_PORT) & 1)) inb(KB_DATA_PORT);

    /* Enable first PS/2 port (Keyboard) */
    kb_wait_write();
    outb(KB_STATUS_PORT, 0xAE);

    /* Read controller configuration byte */
    kb_wait_write();
    outb(KB_STATUS_PORT, 0x20);
    kb_wait_read();
    uint8_t cfg = inb(KB_DATA_PORT);

    /* Enable keyboard IRQ1 (bit 0) and enable keyboard clock (clear bit 4) */
    cfg |= 0x01;
    cfg &= (uint8_t)~0x10;

    /* Write configuration byte back */
    kb_wait_write();
    outb(KB_STATUS_PORT, 0x60);
    kb_wait_write();
    outb(KB_DATA_PORT, cfg);

    /* Enable scanning on keyboard device */
    kb_wait_write();
    outb(KB_DATA_PORT, 0xF4);
    kb_wait_read();
    (void)inb(KB_DATA_PORT); /* read ACK */

    irq_register(KB_IRQ, keyboard_irq_handler);
    serial_print("[KBD] keyboard initialized, IRQ1 enabled\n");
}

bool keyboard_read(char *out)
{
    if (g_ring_tail == g_ring_head) return false;
    *out = (char)g_ring[g_ring_tail];
    g_ring_tail = (g_ring_tail + 1) & (KB_RING_SIZE - 1);
    return true;
}