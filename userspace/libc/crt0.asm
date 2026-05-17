; crt0.asm — C runtime entry point for all Exploidus user programs.
;
; The kernel jumps here via IRETQ. The stack is clean (RSP aligned).
; We call main() and then exit() with its return value.
;
; Linked at the start of every user executable.

bits 64
section .text
global _start
extern main

_start:

    ; Stack is already aligned to 16 bytes by jump_to_userspace.
    ; argc and argv are not yet implemented — pass 0, NULL.
    xor  rdi, rdi    ; argc = 0
    xor  rsi, rsi    ; argv = NULL




    call main

    ; exit(main's return value)
    mov  rdi, rax
    mov  rax, 0      ; SYS_EXIT = 0
    syscall

    ; Should never reach here. Spin if it does.
.hang:
    hlt
    jmp .hang
