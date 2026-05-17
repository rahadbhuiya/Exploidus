bits 64
section .text

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push qword 0        ; fake error code
    push qword %1       ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    ; CPU already pushed: error_code, rip, cs, rflags, rsp, ss
    ; push vector number — error code 
    push qword %1
    jmp isr_common_stub
%endmacro

; ---- CPU exceptions ----
; Exceptions that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
ISR_NOERR  0    ; #DE divide error
ISR_NOERR  1    ; #DB debug
ISR_NOERR  2    ; NMI
ISR_NOERR  3    ; #BP breakpoint
ISR_NOERR  4    ; #OF overflow
ISR_NOERR  5    ; #BR bound range
ISR_NOERR  6    ; #UD invalid opcode
ISR_NOERR  7    ; #NM device not available
ISR_ERR    8    ; #DF double fault        (error code = 0 but CPU pushes it)
ISR_NOERR  9    ; coprocessor overrun (legacy)
ISR_ERR   10    ; #TS invalid TSS
ISR_ERR   11    ; #NP segment not present
ISR_ERR   12    ; #SS stack fault
ISR_ERR   13    ; #GP general protection
ISR_ERR   14    ; #PF page fault
ISR_NOERR 15    ; reserved
ISR_NOERR 16    ; #MF x87 FPU error
ISR_ERR   17    ; #AC alignment check
ISR_NOERR 18    ; #MC machine check
ISR_NOERR 19    ; #XF SIMD FP exception
ISR_NOERR 20    ; #VE virtualization exception
ISR_ERR   21    ; #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    ; #HV (AMD)
ISR_ERR   30    ; #SX (AMD security)
ISR_NOERR 31

;  IRQ vectors 32-47 (PIC remapped) 
%assign i 32
%rep 16
    ISR_NOERR i
%assign i i+1
%endrep

;  Remaining vectors 48-255 
%assign i 48
%rep 208
    ISR_NOERR i
%assign i i+1
%endrep


; COMMON STUB
; All ISRs jump here. Stack at entry:
;   [rsp+0]  = vector
;   [rsp+8]  = error code (real or fake 0)
;   [rsp+16] = rip  (pushed by CPU)
;   [rsp+24] = cs
;   [rsp+32] = rflags
;   [rsp+40] = rsp  (if privilege change)
;   [rsp+48] = ss   (if privilege change)

extern interrupt_handler

isr_common_stub:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument = pointer to interrupt_frame_t on stack
    mov rdi, rsp
    cld
    call interrupt_handler

    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Clean vector + error code off stack
    add rsp, 16

    iretq

; ISR STUB TABLE  — 256 x 8-byte function pointers

section .data
global isr_stub_table

isr_stub_table:
%assign i 0
%rep 256
    dq isr_%+i
%assign i i+1
%endrep