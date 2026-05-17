#pragma once
#include <stdint.h>

// isr_common_stub push order (top of stack = lowest address):
//   r15, r14, r13, r12, r11, r10, r9, r8
//   rbp, rdi, rsi, rdx, rcx, rbx, rax
//   int_num, error_code
//   rip, cs, rflags, rsp, ss  (CPU pushed)
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9,  r8;
    uint64_t rbp, rdi, rsi, rdx;
    uint64_t rcx, rbx, rax;

    uint64_t int_num;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

void idt_init(void);