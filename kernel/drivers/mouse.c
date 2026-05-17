#include "mouse.h"
#include "fb.h"
#include "../arch/x86_64/irq.h"
#include "../drivers/serial.h"
#include <stdint.h>

mouse_state_t g_mouse = {400, 300, false, false, false};

static uint8_t  g_mouse_buf[3];
static uint8_t  g_mouse_idx = 0;

/* Saved pixels under cursor */
static uint32_t g_cursor_save[16*16];
static int32_t  g_cursor_sx = -1;
static int32_t  g_cursor_sy = -1;

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}
static void mouse_wait_write(void)
{
    uint32_t t = 100000;
    while (t-- && (inb(0x64) & 2));
}
static void mouse_wait_read(void)
{
    uint32_t t = 100000;
    while (t-- && !(inb(0x64) & 1));
}
static void mouse_write(uint8_t data)
{
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, data);
}
static uint8_t mouse_read(void)
{
    mouse_wait_read();
    return inb(0x60);
}

/* 16x16 arrow cursor bitmap */
static const uint16_t cursor_shape[16] = {
    0x8000,0xC000,0xE000,0xF000,
    0xF800,0xFC00,0xFE00,0xFF00,
    0xFF80,0xFFC0,0xFC00,0xEC00,
    0xC600,0x0600,0x0300,0x0300,
};

void mouse_draw_cursor(void)
{
    if (!g_fb.active) return;
    int32_t x = g_mouse.x, y = g_mouse.y;
    g_cursor_sx = x; g_cursor_sy = y;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            int32_t px = x+col, py = y+row;
            if (px<0||py<0||(uint32_t)px>=g_fb.width||(uint32_t)py>=g_fb.height) {
                g_cursor_save[row*16+col] = 0;
                continue;
            }
            uint32_t *p = (uint32_t*)(uintptr_t)
                (g_fb.addr + (uint32_t)py*g_fb.pitch + (uint32_t)px*4);
            g_cursor_save[row*16+col] = *p;
            /* White fill with black outline for visibility on any background */
            int set = !!(cursor_shape[row] & (0x8000>>col));
            /* Check any neighbouring pixel in shape for outline */
            int outline = 0;
            if (!set) {
                if (row>0  && (cursor_shape[row-1] & (0x8000>>col)))           outline=1;
                if (row<15 && (cursor_shape[row+1] & (0x8000>>col)))           outline=1;
                if (col>0  && (cursor_shape[row]   & (0x8000>>(col-1))))       outline=1;
                if (col<15 && (cursor_shape[row]   & (0x8000>>(col+1))))       outline=1;
            }
            if (set)    *p = 0xFFFFFF;
            else if (outline) *p = 0x000000;
        }
    }
}

void mouse_erase_cursor(void)
{
    if (!g_fb.active || g_cursor_sx < 0) return;
    int32_t x = g_cursor_sx, y = g_cursor_sy;
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            int32_t px = x+col, py = y+row;
            if (px<0||py<0||(uint32_t)px>=g_fb.width||(uint32_t)py>=g_fb.height) continue;
            uint32_t *p = (uint32_t*)(uintptr_t)
                (g_fb.addr + (uint32_t)py*g_fb.pitch + (uint32_t)px*4);
            *p = g_cursor_save[row*16+col];
        }
    }
    g_cursor_sx = g_cursor_sy = -1;
}

static void mouse_irq_handler(interrupt_frame_t *frame)
{
    (void)frame;

    uint8_t status = inb(0x64);
    if (!(status & 0x20)) return;

    uint8_t data = inb(0x60);
    g_mouse_buf[g_mouse_idx++] = data;

    if (g_mouse_idx < 3) return;
    g_mouse_idx = 0;

    uint8_t flags = g_mouse_buf[0];
    if (!(flags & 0x08)) return;

    mouse_erase_cursor();

    g_mouse.left   = (flags & 0x01) != 0;
    g_mouse.right  = (flags & 0x02) != 0;
    g_mouse.middle = (flags & 0x04) != 0;

    int32_t dx = (int32_t)(int8_t)g_mouse_buf[1];
    int32_t dy = (int32_t)(int8_t)g_mouse_buf[2];

    g_mouse.x += dx;
    g_mouse.y -= dy;

    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= (int32_t)g_fb.width)  g_mouse.x = (int32_t)g_fb.width  - 1;
    if (g_mouse.y >= (int32_t)g_fb.height) g_mouse.y = (int32_t)g_fb.height - 1;

    mouse_draw_cursor();
}

void mouse_init(void)
{
    uint32_t flush = 100;
    while (flush-- && (inb(0x64) & 1)) inb(0x60);
    mouse_wait_write(); outb(0x64, 0xA8);
    mouse_wait_write(); outb(0x64, 0x20);
    mouse_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x02;
    cfg &= (uint8_t)~0x20;
    mouse_wait_write(); outb(0x64, 0x60);
    mouse_wait_write(); outb(0x60, cfg);
    mouse_write(0xF6); mouse_read();
    mouse_write(0xF4); mouse_read();
    irq_register(12, mouse_irq_handler);
    serial_print("[MOUSE] PS/2 mouse initialized\n");
    mouse_draw_cursor();  /* show cursor immediately at boot */
}
