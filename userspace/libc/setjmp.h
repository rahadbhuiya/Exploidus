#pragma once

/*
 * setjmp/longjmp — x86-64 System V ABI implementation.
 *
 * Needed by pretty much any language interpreter you'd port to
 * Exploidus (Lua's pcall/error, many others) since C has no native
 * exception mechanism and interpreters rely on setjmp/longjmp to
 * unwind on error instead.
 *
 * Saves the callee-saved registers (rbx, rbp, r12-r15), the stack
 * pointer, and the return address — exactly what's needed to resume
 * execution at the setjmp() call site from anywhere later on the
 * same stack.
 */

typedef struct {
    unsigned long rbx;
    unsigned long rbp;
    unsigned long r12;
    unsigned long r13;
    unsigned long r14;
    unsigned long r15;
    unsigned long rsp;
    unsigned long rip;
} jmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));