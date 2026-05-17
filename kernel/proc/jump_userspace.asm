bits 64

section .text
global jump_to_userspace

jump_to_userspace:
    ; rdi = entry, rsi = stack_top, rdx = pml4
    cli
    cld

    mov  rax, rsi
    and  rax, -16

    push qword 0x1B   ; SS  = user data  (GDT[3] | 3 = 0x18|3)
    push rax           ; RSP = user stack
    push qword 0x202  ; RFLAGS = IF+reserved
    push qword 0x23   ; CS  = user code  (GDT[4] | 3 = 0x20|3)
    push rdi           ; RIP = entry point

    mov  cr3, rdx     ; switch to process page table

    mov  ax, 0x1B     ; user data selector
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    xor  rax, rax
    xor  rbx, rbx
    xor  rdx, rdx
    xor  rsi, rsi
    xor  rdi, rdi
    xor  rbp, rbp
    xor  r8,  r8
    xor  r9,  r9
    xor  r10, r10
    xor  r11, r11
    xor  r12, r12
    xor  r13, r13
    xor  r14, r14
    xor  r15, r15

    iretq
