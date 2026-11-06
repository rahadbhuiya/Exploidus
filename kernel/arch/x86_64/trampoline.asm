bits 16
section .text

global trampoline_start
global trampoline_end
global trampoline_cr3
global trampoline_stack
global trampoline_entry

trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary 32-bit GDT using 32-bit address override
    o32 lgdt [0x8000 + (t_gdt_ptr - trampoline_start)]

    ; Set PE bit in CR0
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit protected mode
    jmp dword 0x08:(0x8000 + (trampoline_32 - trampoline_start))

bits 32
trampoline_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable PAE in CR4
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load CR3 with kernel PML4
    mov eax, [0x8000 + (trampoline_cr3 - trampoline_start)]
    mov cr3, eax

    ; Enable Long Mode & NXE in IA32_EFER MSR (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ; Enable Paging (PG) and Protected Mode (PE) in CR0
    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    ; Far jump to 64-bit long mode
    jmp 0x18:(0x8000 + (trampoline_64 - trampoline_start))

bits 64
trampoline_64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Load per-CPU kernel stack pointer
    mov rsp, [0x8000 + (trampoline_stack - trampoline_start)]

    ; Call 64-bit C entry point
    mov rax, [0x8000 + (trampoline_entry - trampoline_start)]
    call rax

.ap_halt:
    cli
    hlt
    jmp .ap_halt

align 16
t_gdt:
    dq 0x0000000000000000 ; 0x00: Null descriptor
    dq 0x00CF9A000000FFFF ; 0x08: 32-bit Code (D=1, L=0)
    dq 0x00CF92000000FFFF ; 0x10: 32-bit Data (W=1)
    dq 0x00AF9A000000FFFF ; 0x18: 64-bit Code (L=1, D=0)
t_gdt_end:

align 4
t_gdt_ptr:
    dw t_gdt_end - t_gdt - 1
    dd 0x8000 + (t_gdt - trampoline_start)

align 8
trampoline_cr3:    dd 0
trampoline_stack:  dq 0
trampoline_entry:  dq 0

align 16
trampoline_end:
