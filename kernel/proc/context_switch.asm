bits 64
section .text
global context_switch

; void context_switch(process_t *prev, process_t *next)
; context is at offset 16 within process_t
; cr3 is at offset 80
;
; Two cases for next->context.rip:
;   1. First run:  proc_trampoline address (set by sys_spawn)
;                  context.rsp = kernel_stack_top (empty stack)
;                  → jmp to trampoline
;   2. Resume:     return address saved by previous context_switch
;                  context.rsp = kernel stack with return addr on top
;                  → restore rsp, then ret (return addr already on stack)

context_switch:

    ; SAVE CURRENT CONTEXT  (rdi = prev process_t*)

    mov [rdi + 16], r15
    mov [rdi + 24], r14
    mov [rdi + 32], r13
    mov [rdi + 40], r12
    mov [rdi + 48], rbx
    mov [rdi + 56], rbp

    ; rsp currently points to return address of context_switch call
    mov [rdi + 64], rsp

    ; Mark rip as 0 — we will resume via ret (return addr is on stack)
    mov qword [rdi + 72], 0

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

    ; CHECK: first run or resume?
    mov rax, [rsi + 72]
    test rax, rax
    jz .resume              ; rip == 0 → resume via ret

    ; First run — jump to proc_trampoline
    cmp rax, 0x1000
    jb .panic
    jmp rax

.resume:
    ; Return address is on top of restored stack
    ret

.panic:
    cli
.hang:
    hlt
    jmp .hang