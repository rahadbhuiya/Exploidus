bits 64
section .text._start
global _start
extern main
extern __bss_end


section .text._start
_start:
    ; Zero BSS using RIP-relative addresses (ASLR safe)
    lea  rdi, [rel __bss_start_here]
    lea  rcx, [rel __bss_end]
    sub  rcx, rdi
    jle  .done_bss
    xor  al, al
    rep  stosb
.done_bss:
    ; System V ABI: kernel placed argc at rsp, argv at rsp+8
    mov  rdi, [rsp]            ; argc
    lea  rsi, [rsp + 8]        ; argv
    lea  rdx, [rsi + rdi*8 + 8] ; envp
    call main
    ; exit syscall (SYS_EXIT = 0) — propagate main()'s real return
    ; value (was hardcoded to 0 before, silently discarding it; Lua
    ; and most real programs rely on this to signal success/failure)
    mov  rdi, rax
    xor  rax, rax      ; SYS_EXIT = 0
    syscall
.hang:
    hlt
    jmp  .hang

section .bss
global __bss_start_here
__bss_start_here: resb 0