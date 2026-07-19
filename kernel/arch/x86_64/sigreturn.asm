bits 64
section .text

global sigreturn_restore

; void sigreturn_restore(interrupt_frame_t *frame)  -- never returns.
;
; rdi = pointer to a saved interrupt_frame_t (see idt.h for the exact
; field order; this function's pop sequence depends on it matching
; exactly, the same way isr_common_stub's epilogue does).
;
; sys_sigreturn() reaches this through the ordinary SYSCALL path, but
; that path's own epilogue (kernel/syscall/entry.asm) only restores a
; handful of registers before sysret. To resume arbitrary interrupted
; code we need every register back, including rip/rsp/rflags/cs/ss, so
; instead of returning normally we point rsp at the saved frame, pop
; everything out of it in the same order isr_common_stub would, and
; iretq straight back to the instruction the fault interrupted.
sigreturn_restore:
    mov  rsp, rdi

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    ; Skip int_num + error_code, they are not part of the CPU's iret frame
    add  rsp, 16

    iretq