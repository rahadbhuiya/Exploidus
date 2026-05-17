bits 64

section .bss
global syscall_user_rsp
syscall_user_rsp: resq 1

section .text
global syscall_entry_stub
global syscall_init_msr
extern syscall_dispatch
extern g_syscall_kernel_rsp

syscall_init_msr:
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, 1
    wrmsr

    mov  ecx, 0xC0000081
    xor  eax, eax
    mov  edx, 0x00100008
    wrmsr

    mov  ecx, 0xC0000082
    lea  rax, [rel syscall_entry_stub]
    mov  rdx, rax
    shr  rdx, 32
    wrmsr

    mov  ecx, 0xC0000084
    mov  eax, 0x200
    xor  edx, edx
    wrmsr
    ret

syscall_entry_stub:
    mov  [rel syscall_user_rsp], rsp
    mov  rsp, [rel g_syscall_kernel_rsp]

    push qword [rel syscall_user_rsp]
    push rcx
    push r11
    push rax
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    mov  rdi, rsp
    cld
    call syscall_dispatch

    pop  r9
    pop  r8
    pop  r10
    pop  rdx
    pop  rsi
    pop  rdi
    pop  rax
    pop  r11
    pop  rcx
    pop  rsp

    o64 sysret
