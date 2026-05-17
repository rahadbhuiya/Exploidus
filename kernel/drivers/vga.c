#include "vga.h"
#include "fb_console.h"

#define VGA_BASE    ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

#define VGA_COLOR(fg, bg) ((uint8_t)((bg << 4) | (fg & 0x0F)))
#define VGA_CHAR(c, color) ((uint16_t)((uint16_t)(color) << 8 | (uint8_t)(c)))

/* VGA color codes */
#define VGA_BLACK   0
#define VGA_BLUE    1
#define VGA_GREEN   2
#define VGA_CYAN    3
#define VGA_RED     4
#define VGA_MAGENTA 5
#define VGA_BROWN   6
#define VGA_LGRAY   7
#define VGA_DGRAY   8
#define VGA_LBLUE   9
#define VGA_LGREEN  10
#define VGA_LCYAN   11
#define VGA_LRED    12
#define VGA_LMAGENTA 13
#define VGA_YELLOW  14
#define VGA_WHITE   15

/* I/O port for VGA cursor control */
#define VGA_CTRL_REG  0x3D4
#define VGA_DATA_REG  0x3D5

static uint8_t  g_color = 0;
static uint32_t g_row   = 0;
static uint32_t g_col   = 0;

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void update_cursor(void)
{
    uint16_t pos = (uint16_t)(g_row * VGA_WIDTH + g_col);
    outb(VGA_CTRL_REG, 0x0F);
    outb(VGA_DATA_REG, (uint8_t)(pos & 0xFF));
    outb(VGA_CTRL_REG, 0x0E);
    outb(VGA_DATA_REG, (uint8_t)(pos >> 8));
}

static void scroll(void)
{
    volatile uint16_t *vga = VGA_BASE;
    uint16_t blank = VGA_CHAR(' ', g_color);

    for (uint32_t row = 1; row < VGA_HEIGHT; row++) {
        for (uint32_t col = 0; col < VGA_WIDTH; col++) {
            vga[(row - 1) * VGA_WIDTH + col] = vga[row * VGA_WIDTH + col];
        }
    }
    for (uint32_t col = 0; col < VGA_WIDTH; col++) {
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
    }

    if (g_row > 0) g_row--;
}

void vga_init(void)
{
    g_color = VGA_COLOR(VGA_LGREEN, VGA_BLACK);
    g_row   = 0;
    g_col   = 0;
    vga_clear();
}

void vga_clear(void)
{
    volatile uint16_t *vga   = VGA_BASE;
    uint16_t           blank = VGA_CHAR(' ', g_color);

    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = blank;

    g_row = 0;
    g_col = 0;
    update_cursor();
}

void vga_putc(char c)
{
    volatile uint16_t *vga = VGA_BASE;

    if (c == '\n') {
        g_col = 0;
        g_row++;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\t') {
        g_col = (g_col + 8) & ~7u;
        if (g_col >= VGA_WIDTH) {
            g_col = 0;
            g_row++;
        }
    } else if (c == '\b') {
        if (g_col > 0) {
            g_col--;
            vga[g_row * VGA_WIDTH + g_col] = VGA_CHAR(' ', g_color);
        }
    } else {
        vga[g_row * VGA_WIDTH + g_col] = VGA_CHAR(c, g_color);
        g_col++;
        if (g_col >= VGA_WIDTH) {
            g_col = 0;
            g_row++;
        }
    }

    if (g_row >= VGA_HEIGHT) scroll();
    update_cursor();
    fb_console_putc(c);
}

void vga_puts(const char *s)
{
    while (*s) vga_putc(*s++);
}

void vga_puthex(uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[18];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[17 - i] = hex[val & 0xF], val >>= 4;
    buf[17] = '\0';
    vga_puts(buf);
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    g_color = VGA_COLOR(fg, bg);
}
