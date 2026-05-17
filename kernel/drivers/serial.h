#pragma once
#include <stdint.h>
#include <stdbool.h>

void serial_init(void);
void serial_putc(char c);
void serial_print(const char *s);
void serial_printhex(uint64_t val);
bool serial_read_byte(char *out);
