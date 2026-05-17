bits 64
section .text
global context_switch

; void context_switch(process_t *prev, process_t *next)
; context is at offset 16 within process_t
; cr3 is at offset 80

context_switch:

    
    ; SAVE CURRENT CONTEXT  (rdi = prev process_t*)
    

    mov [rdi + 16], r15
    mov [rdi + 24], r14
    mov [rdi + 32], r13
    mov [rdi + 40], r12
    mov [rdi + 48], rbx
    mov [rdi + 56], rbp

    mov rax, rsp
    mov [rdi + 64], rax

    mov rax, [rsp]
    mov [rdi + 72], rax

    
    ; LOAD NEXT CONTEXT  (rsi = next process_t*)
    

    mov r15, [rsi + 16]
    mov r14, [rsi + 24]
    mov r13, [rsi + 32]
    mov r12, [rsi + 40]
    mov rbx, [rsi + 48]
    mov rbp, [rsi + 56]

    mov rsp, [rsi + 64]

    
    ; SWITCH ADDRESS SPACE
    

    mov rcx, [rsi + 80]    ; next->cr3
    test rcx, rcx
    jz .skip_cr3
    mov cr3, rcx
.skip_cr3:

    
    ; RIP VALIDATION + JUMP
    

    mov rax, [rsi + 72]

    test rax, rax
    jz .panic

    cmp rax, 0x100000
    jb .panic

    jmp rax

.panic:
    cli
.hang:
    hlt
    jmp .hang
