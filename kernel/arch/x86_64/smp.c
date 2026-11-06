#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "fpu.h"
#include "../../mm/vmm.h"
#include "../../drivers/serial.h"
#include <string.h>

extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];
extern uint32_t trampoline_cr3;
extern uint64_t trampoline_stack;
extern uint64_t trampoline_entry;

smp_core_t g_smp_cores[SMP_MAX_CORES];
uint32_t   g_smp_online_count = 1;

static uint8_t g_ap_stacks[SMP_MAX_CORES][SMP_STACK_SIZE] __attribute__((aligned(16)));

#define TRAMPOLINE_PHYS 0x8000
#define TRAMPOLINE_VECTOR 0x08

static void delay_io(uint32_t iterations)
{
    for (volatile uint32_t i = 0; i < iterations; i++) {
        __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
    }
}

uint32_t smp_get_current_cpu(void)
{
    uint8_t lapic_id = apic_get_id();
    for (uint32_t i = 0; i < g_smp_online_count; i++) {
        if (g_smp_cores[i].apic_id == lapic_id) {
            return g_smp_cores[i].cpu_id;
        }
    }
    return 0;
}

uint32_t smp_get_core_count(void)
{
    return g_smp_online_count;
}

bool smp_is_bsp(void)
{
    return smp_get_current_cpu() == 0;
}

void smp_ap_entry(void)
{
    /* Load Kernel 64-bit GDT and IDT */
    gdt_flush_ap();
    idt_load();

    /* Enable Local APIC on this core */
    apic_init_ap();

    /* Initialize FPU/SSE for userspace math/crypto */
    fpu_init();

    uint8_t my_apic = apic_get_id();

    /* Find our core slot */
    uint32_t my_cpu = 0;
    for (uint32_t i = 0; i < ACPI_MAX_CORES; i++) {
        if (g_acpi_info.core_apic_ids[i] == my_apic) {
            my_cpu = i;
            break;
        }
    }

    g_smp_cores[my_cpu].cpu_id = my_cpu;
    g_smp_cores[my_cpu].apic_id = my_apic;
    g_smp_cores[my_cpu].is_bsp = false;

    /* Signal BSP that this core is online */
    g_smp_cores[my_cpu].online = true;

    serial_print("[smp] Application Processor online: CPU #");
    serial_printdec(my_cpu);
    serial_print(" (APIC ID ");
    serial_printdec(my_apic);
    serial_print(")\n");

    /* Enable interrupts and enter AP idle loop */
    __asm__ volatile ("sti");

    while (1) {
        __asm__ volatile ("pause; hlt");
    }
}

void smp_init(void)
{
    uint32_t total_detected = acpi_get_core_count();
    if (total_detected <= 1) {
        serial_print("[smp] Single-core system detected, SMP initialization complete\n");
        return;
    }

    uint8_t bsp_apic = apic_get_id();

    /* Configure BSP core entry */
    g_smp_cores[0].cpu_id = 0;
    g_smp_cores[0].apic_id = bsp_apic;
    g_smp_cores[0].is_bsp = true;
    g_smp_cores[0].online = true;
    g_smp_online_count = 1;

    serial_print("[smp] Initializing SMP: detected ");
    serial_printdec(total_detected);
    serial_print(" cores (BSP APIC ID: ");
    serial_printdec(bsp_apic);
    serial_print(")\n");

    /* Copy AP trampoline to physical address 0x8000 */
    size_t tramp_size = (size_t)(trampoline_end - trampoline_start);
    memcpy((void *)(uintptr_t)TRAMPOLINE_PHYS, trampoline_start, tramp_size);

    /* Set up shared trampoline arguments */
    uint32_t *p_cr3 = (uint32_t *)(uintptr_t)(TRAMPOLINE_PHYS + (uintptr_t)((uint8_t *)&trampoline_cr3 - trampoline_start));
    uint64_t *p_stack = (uint64_t *)(uintptr_t)(TRAMPOLINE_PHYS + (uintptr_t)((uint8_t *)&trampoline_stack - trampoline_start));
    uint64_t *p_entry = (uint64_t *)(uintptr_t)(TRAMPOLINE_PHYS + (uintptr_t)((uint8_t *)&trampoline_entry - trampoline_start));

    *p_cr3 = (uint32_t)vmm_get_kernel_pml4();
    *p_entry = (uint64_t)(uintptr_t)&smp_ap_entry;

    /* Boot each detected secondary processor core */
    for (uint32_t i = 0; i < total_detected; i++) {
        uint8_t target_apic = g_acpi_info.core_apic_ids[i];
        if (target_apic == bsp_apic) {
            continue; /* Skip BSP */
        }

        g_smp_cores[i].cpu_id = i;
        g_smp_cores[i].apic_id = target_apic;
        g_smp_cores[i].is_bsp = false;
        g_smp_cores[i].online = false;

        /* Assign top of dedicated per-core stack */
        *p_stack = (uint64_t)(uintptr_t)&g_ap_stacks[i][SMP_STACK_SIZE];

        /* Step 1: Send INIT IPI (Assert) */
        apic_send_ipi(target_apic, 0x00004500);
        delay_io(20000); /* ~10 ms delay */

        /* Step 2: Send Startup IPI (SIPI) with vector 0x08 -> 0x8000 */
        apic_send_ipi(target_apic, 0x00004600 | TRAMPOLINE_VECTOR);
        delay_io(500); /* ~200 us delay */

        /* Step 3: Send second SIPI */
        apic_send_ipi(target_apic, 0x00004600 | TRAMPOLINE_VECTOR);

        /* Wait up to 50ms for AP to come online */
        for (int timeout = 0; timeout < 500; timeout++) {
            if (g_smp_cores[i].online) {
                g_smp_online_count++;
                break;
            }
            delay_io(200);
        }

        if (!g_smp_cores[i].online) {
            serial_print("[smp] Warning: Core #");
            serial_printdec(i);
            serial_print(" (APIC ID ");
            serial_printdec(target_apic);
            serial_print(") failed to come online\n");
        }
    }

    serial_print("[smp] SMP online: ");
    serial_printdec(g_smp_online_count);
    serial_print("/");
    serial_printdec(total_detected);
    serial_print(" CPU cores active\n");
}
