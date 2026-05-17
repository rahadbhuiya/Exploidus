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

static void keyboard_irq_handler(interrupt_frame_t *frame)
{
    (void)frame;

    /* Ignore mouse data (PS/2 bit 5 = aux/mouse) */
    uint8_t status = inb(KB_STATUS_PORT);
    if (status & 0x20) { inb(KB_DATA_PORT); return; }

    uint8_t scancode = inb(KB_DATA_PORT);
    /* Key release: bit 7 set */
    if (scancode & 0x80) {
        uint8_t key = scancode & 0x7F;
        if (key == 0x2A || key == 0x36) g_shift = false;
        return;
    }
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
    uint32_t next = (g_ring_head + 1) & (KB_RING_SIZE - 1);
    if (next != g_ring_tail) {
        g_ring[g_ring_head] = (uint8_t)c;
        g_ring_head = next;
    }
}

void keyboard_init(void)
{
    serial_print("[KBD] registering IRQ1\n");
    irq_register(KB_IRQ, keyboard_irq_handler);
}

bool keyboard_read(char *out)
{
    if (g_ring_tail == g_ring_head) return false;
    *out = (char)g_ring[g_ring_tail];
    g_ring_tail = (g_ring_tail + 1) & (KB_RING_SIZE - 1);
    return true;
}
