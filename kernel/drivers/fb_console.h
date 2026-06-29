#pragma once
void fb_console_init(void);
void fb_console_putc(char c);
void fb_console_puts(const char *s);
void fb_console_enable(void);    /* re-enable text console output   */
void fb_console_disable(void);   /* suppress output (GUI mode)      */
int  fb_console_enabled(void);   /* returns 1 if enabled            */