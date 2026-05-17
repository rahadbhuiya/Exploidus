; Exploidus — Boot Entry Point
; Maps 0-4GB identity with 2MB pages.
; Upper page-table levels keep USER set so later per-process PML4s can
; place user pages in the low half. Kernel huge-page leaves stay supervisor.

bits 32

MULTIBOOT2_HEADER_MAGIC equ 0xE85250D6
MULTIBOOT2_ARCH_X86     equ 0
HEADER_LENGTH           equ (header_end - header_start)
HEADER_CHECKSUM         equ (0x100000000 - (MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_ARCH_X86 + HEADER_LENGTH))

; Page-table entry flags
PTE_KERN  equ 0x03  ; present + write (no user) — for kernel PML4[0] top entry
PTE_USER  equ 0x07  ; present + write + user    — for PDPT, PD, PT entries
PTE_HUGE  equ 0x83  ; present + write + huge (supervisor leaf)

MSR_EFER  equ 0xC0000080
EFER_LME  equ (1 << 8)
EFER_NXE  equ (1 << 11)
CR0_PE    equ (1 << 0)
CR0_PG    equ (1 << 31)
CR4_PAE   equ (1 << 5)


section .multiboot2
header_start:
    dd MULTIBOOT2_HEADER_MAGIC
    dd MULTIBOOT2_ARCH_X86
    dd HEADER_LENGTH
    dd HEADER_CHECKSUM
    ; Framebuffer tag (type 5)
    align 8
    dw 5        ; type
    dw 0        ; flags (optional)
    dd 20       ; size
    dd 800      ; width
    dd 600      ; height
    dd 32       ; depth (bpp)
    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .bss
align 4096
pml4_table: resb 4096
pdpt_table: resb 4096
pd0:        resb 4096    ;  0 -  1 GB
pd1:        resb 4096    ;  1 -  2 GB
pd2:        resb 4096    ;  2 -  3 GB
pd3:        resb 4096    ;  3 -  4 GB
boot_stack_bottom: resb 32768
boot_stack_top:

section .data
align 8
boot_gdt:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF  ; 0x08 kernel code 64-bit
    dq 0x00AF92000000FFFF  ; 0x10 kernel data
    dq 0x00AFF2000000FFFF  ; 0x18 user   data          (DPL=3)
    dq 0x00AFFA000000FFFF  ; 0x20 user   code 64-bit   (DPL=3)
boot_gdt_end:
boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dq boot_gdt

section .text
global _start
extern kernel_main

_start:
    cli
    mov edi, eax        ; multiboot magic
    mov esi, ebx        ; multiboot info ptr
    mov esp, boot_stack_top

    cmp edi, 0x36D76289
    jne boot_halt

    call setup_paging
    lgdt [boot_gdt_ptr]

    mov eax, cr4
    or  eax, CR4_PAE
    mov cr4, eax

    mov eax, pml4_table
    mov cr3, eax

    mov ecx, MSR_EFER
    rdmsr
    or  eax, EFER_LME | EFER_NXE
    wrmsr

    mov eax, cr0
    or  eax, CR0_PG | CR0_PE
    mov cr0, eax

    jmp 0x08:boot_long64

boot_halt:
    hlt
    jmp boot_halt


; setup_paging — identity map 0-4 GB using 2 MB huge pages.
; Huge-page leaf entries are supervisor-only; user pages are mapped later.

setup_paging:
    ; PML4[0] → PDPT  (USER bit so user code can walk this entry)
    mov eax, pdpt_table
    or  eax, PTE_USER
    mov [pml4_table], eax

    ; PDPT[0..3] → PD0..PD3  (USER bit)
    mov eax, pd0
    or  eax, PTE_USER
    mov [pdpt_table + 0*8], eax

    mov eax, pd1
    or  eax, PTE_USER
    mov [pdpt_table + 1*8], eax

    mov eax, pd2
    or  eax, PTE_USER
    mov [pdpt_table + 2*8], eax

    mov eax, pd3
    or  eax, PTE_USER
    mov [pdpt_table + 3*8], eax

    ; PD0:  0x00000000 – 0x3FFFFFFF   (0–1 GB)
    xor ecx, ecx
sp_map0:
    cmp ecx, 512
    jge sp_done0
    mov eax, ecx
    shl eax, 21
    or  eax, PTE_HUGE
    mov [pd0 + ecx*8], eax
    inc ecx
    jmp sp_map0
sp_done0:

    ; PD1:  0x40000000 – 0x7FFFFFFF   (1–2 GB)
    xor ecx, ecx
sp_map1:
    cmp ecx, 512
    jge sp_done1
    mov eax, ecx
    shl eax, 21
    add eax, 0x40000000
    or  eax, PTE_HUGE
    mov [pd1 + ecx*8], eax
    inc ecx
    jmp sp_map1
sp_done1:

    ; PD2:  0x80000000 – 0xBFFFFFFF   (2–3 GB)
    xor ecx, ecx
sp_map2:
    cmp ecx, 512
    jge sp_done2
    mov eax, ecx
    shl eax, 21
    add eax, 0x80000000
    or  eax, PTE_HUGE
    mov [pd2 + ecx*8], eax
    inc ecx
    jmp sp_map2
sp_done2:

    ; PD3:  0xC0000000 – 0xFFFFFFFF   (3–4 GB, covers MMIO at 0xFEBC0000)
    xor ecx, ecx
sp_map3:
    cmp ecx, 512
    jge sp_done3
    mov eax, ecx
    shl eax, 21
    add eax, 0xC0000000
    or  eax, PTE_HUGE
    mov [pd3 + ecx*8], eax
    inc ecx
    jmp sp_map3
sp_done3:
    ret


; 64-bit entry

bits 64
boot_long64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, boot_stack_top
    call kernel_main
boot_hang:
    cli
    hlt
    jmp boot_hang
