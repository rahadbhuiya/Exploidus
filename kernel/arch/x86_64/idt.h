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

/*
 * sigreturn_restore(frame) -- kernel/arch/x86_64/sigreturn.asm.
 * Restores every register (including rip/rsp/rflags/cs/ss) from a
 * saved interrupt_frame_t and iret's straight back into userspace,
 * bypassing the normal syscall return path in kernel/syscall/entry.asm
 * (which only restores a handful of registers, not enough to resume
 * arbitrary interrupted code). Used by sys_sigreturn() in
 * kernel/syscall/table.c to resume the code a signal handler
 * interrupted. Never returns.
 */
void sigreturn_restore(interrupt_frame_t *frame) __attribute__((noreturn));