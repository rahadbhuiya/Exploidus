bits 64
section .text
global setjmp
global longjmp

; int setjmp(jmp_buf env)     -- rdi = env
; jmp_buf layout (offsets, 8 bytes each):
;   0  rbx   8  rbp   16 r12   24 r13
;   32 r14   40 r15   48 rsp   56 rip
setjmp:
    mov [rdi+0],  rbx
    mov [rdi+8],  rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+8]        ; caller's rsp (after this call's return addr)
    mov [rdi+48], rax
    mov rax, [rsp]          ; return address (where setjmp() was called)
    mov [rdi+56], rax
    xor eax, eax            ; setjmp() returns 0 on the direct call
    ret

; void longjmp(jmp_buf env, int val) -- rdi = env, rsi = val
; Never returns: restores env's saved state, then jumps back to the
; setjmp() call site, whose call to setjmp() will now appear to
; return `val` (or 1 if val == 0).
longjmp:
    mov eax, esi
    test eax, eax
    jnz .val_ok
    mov eax, 1
.val_ok:
    mov rbx, [rdi+0]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    mov rcx, [rdi+56]
    jmp rcx