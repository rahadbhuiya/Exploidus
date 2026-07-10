#include "idt.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "../../proc/process.h"
#include "../../proc/scheduler.h"
#include <stdint.h>
#include <string.h>

#define IDT_ENTRIES 256

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

#define IDT_TYPE_INTERRUPT_GATE 0x8E  /* present, ring 0, interrupt gate */
#define IDT_TYPE_TRAP_GATE      0x8F  /* present, ring 0, trap gate */

static idt_entry_t g_idt[IDT_ENTRIES];
static idt_ptr_t   g_idt_ptr;

/* ISR stubs declared in isr.asm */
extern void *isr_stub_table[IDT_ENTRIES];

void idt_set_handler(uint8_t vector, void *handler, uint8_t ist)
{
    uint64_t addr = (uint64_t)(uintptr_t)handler;

    g_idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    g_idt[vector].selector    = 0x08;   /* kernel code segment */
    g_idt[vector].ist         = ist & 0x7;
    g_idt[vector].type_attr   = IDT_TYPE_INTERRUPT_GATE;
    g_idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero        = 0;
}

static const char *g_exception_names[32] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

void exception_handler(interrupt_frame_t *frame)
{
    uint64_t vec = frame->int_num;

    /* RPL (bottom 2 bits of CS) == 3 means the fault happened while
     * running userspace code, not kernel code. */
    int user_fault = (frame->cs & 0x3) == 0x3;

    serial_print("\n[EXPLOIDUS] Exception: ");
    if (vec < 32)
        serial_print(g_exception_names[vec]);
    else
        serial_print("Unknown");
    serial_print(user_fault ? " (userspace)\n" : " (KERNEL)\n");

    serial_print("RIP="); serial_printhex(frame->rip); serial_print("\n");
    serial_print("RSP="); serial_printhex(frame->rsp); serial_print("\n");
    serial_print("RAX="); serial_printhex(frame->rax); serial_print("\n");
    serial_print("ERR="); serial_printhex(frame->error_code); serial_print("\n");
    if (vec == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_print("CR2="); serial_printhex(cr2); serial_print("\n");
    }

    /*
     * Crash isolation: a fault caused by a userspace process (bad
     * pointer, divide-by-zero, illegal instruction, etc.) should only
     * kill that one process — the same way sys_exit() does — instead
     * of taking the whole kernel down. A fault while running KERNEL
     * code is a different story: kernel state may be corrupted, so
     * there's nothing safe to do except halt, same as before.
     */
    if (user_fault && g_current_proc) {
        /*
         * Signal delivery: map the fault vector to the signal a real
         * Unix would raise for it, and if the process registered a
         * handler (via signal()), redirect execution there instead
         * of killing it outright — same as real crash-handler
         * semantics (log-and-exit, custom cleanup, etc.).
         *
         * Deliberate limitation: this does NOT support a handler
         * that returns normally to resume the faulting code (that
         * needs a full sigreturn trampoline, a bigger and riskier
         * change) — a registered handler here is expected to end by
         * calling exit() itself. If it doesn't, whatever runs after
         * it falling off the end is undefined. Clear the handler
         * before jumping to it so a second fault inside the handler
         * itself falls through to the normal kill path instead of
         * looping.
         */
        int sig = 0;
        switch (vec) {
            case 0:  sig = 8;  break; /* divide error -> SIGFPE  */
            case 6:  sig = 4;  break; /* invalid opcode -> SIGILL */
            case 13: sig = 11; break; /* GP fault -> SIGSEGV */
            case 14: sig = 11; break; /* page fault -> SIGSEGV */
            default: sig = 0;  break; /* no mapping -> default kill */
        }

        if (sig > 0 && sig < 16 && g_current_proc->sig_handlers[sig] != 0) {
            uint64_t handler = g_current_proc->sig_handlers[sig];
            g_current_proc->sig_handlers[sig] = 0; /* one-shot, see above */

            serial_print("[EXPLOIDUS] Delivering signal ");
            serial_printhex((uint64_t)sig);
            serial_print(" to PID=");
            serial_printhex(g_current_proc->pid);
            serial_print(" handler=");
            serial_printhex(handler);
            serial_print("\n");

            frame->rdi = (uint64_t)sig; /* handler's first arg = signal number */
            frame->rip = handler;
            return; /* resumes userspace at the handler via IRET */
        }

        serial_print("[EXPLOIDUS] Killing faulting process PID=");
        serial_printhex(g_current_proc->pid);
        serial_print(" — rest of the system continues.\n");

        proc_exit(g_current_proc->pid, -(int)vec);
        sched_yield();

        /* sched_yield() only returns here if, for whatever reason,
         * this (now-zombie) process is still "current" — it will
         * never be scheduled again, so just wait for the timer IRQ
         * to switch away, exactly like sys_exit() does. */
        __asm__ volatile ("sti");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    vga_puts("\n\n  *** KERNEL PANIC ***\n");
    vga_puts("  Exception: ");
    if (vec < 32) vga_puts(g_exception_names[vec]);
    vga_puts("\n  System halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

extern void irq_dispatch(interrupt_frame_t *frame);

void interrupt_handler(interrupt_frame_t *frame)
{
    if (frame->int_num < 32) {
        exception_handler(frame);
    } else if (frame->int_num < 48) {
        irq_dispatch(frame);
    }
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));

    for (int i = 0; i < IDT_ENTRIES; i++) {
        if (!isr_stub_table[i]) continue;

        /*
         * Vector 8 = Double Fault — must use IST=1 so the CPU switches
         * to the dedicated df_stack defined in gdt.c (g_tss.ist[0]).
         * Without IST, a double fault with a corrupt kernel stack causes
         * an immediate triple fault before the handler can print anything.
         */
        uint8_t ist = (i == 8) ? 1 : 0;
        idt_set_handler((uint8_t)i, isr_stub_table[i], ist);
    }

    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (uint64_t)(uintptr_t)g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idt_ptr));
    __asm__ volatile ("sti");
}