; kernel/arch/x86_64/gdt_flush.asm
bits 64
global gdt_flush

gdt_flush:
    ; rdi = pointer to gdt_ptr_t
    lgdt [rdi]

    ; reload CS via far return
    push qword 0x08         ; kernel code selector
    lea  rax, [rel .reload]
    push rax
    retfq                   ; far return — flushes CS

.reload:
    mov ax, 0x10            ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret