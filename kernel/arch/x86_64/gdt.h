#pragma once
#include <stdint.h>

void gdt_init(void);

/* FIX: scheduler needs this */
void tss_set_rsp0(uint64_t rsp0);
void gdt_dump_tss_descriptor(void);
extern uint64_t g_syscall_kernel_rsp;
