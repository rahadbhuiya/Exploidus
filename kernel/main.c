#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/irq.h"
#include "arch/x86_64/apic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "cap/capability.h"
#include "cap/broker.h"
#include "proc/process.h"
#include "proc/scheduler.h"
#include "proc/fork_exec.h"
#include "syscall/table.h"
extern void syscall_init_msr(void);
#include "audit/audit.h"
#include "cnsl/cnsl.h"
#include "cnsl/fim.h"
#include "huddlecluster/huddlecluster.h"
#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/font.h"
#include "drivers/fb_console.h"
#include "drivers/mouse.h"
#include "drivers/serial.h"
#include "drivers/ata.h"
#include "fs/vfs/vfs.h"
#include "fs/exfs/exfs.h"
#include "elf/elf.h"
#include "net/netstack.h"
#include "include/multiboot2.h"
#include "ipc/ipc.h"
#include "shm/shm.h"
#include <stdint.h>
#include <string.h>

extern process_t *g_current_proc;

/*  Multiboot2 memory map parser  */

static void parse_framebuffer(uint64_t mb_info_phys)
{
    if (!mb_info_phys) return;
    mb2_info_t *info = (mb2_info_t *)(uintptr_t)mb_info_phys;
    uint8_t *ptr = (uint8_t *)(info + 1);
    uint8_t *end = (uint8_t *)info + info->total_size;
    while (ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t *)ptr;
        if (tag->type == 0) break;
        if (tag->type == MB2_TAG_FB) {
            mb2_tag_fb_t *fb = (mb2_tag_fb_t *)tag;
            if (fb->bpp == 32 && fb->fb_type == 1) {
                fb_init(fb->addr, fb->width, fb->height, fb->pitch, fb->bpp);
                fb_back_init();
                serial_print("[FB] init addr=");
                serial_printhex(fb->addr);
                serial_print("\n");
            }
            break;
        }
        ptr += MB2_ALIGN(tag->size);
    }
}

static void parse_memory_map(uint64_t mb_info_phys,
                              uint64_t *out_base, uint64_t *out_size)
{
    *out_base = 0x200000;
    *out_size = 0x4000000;

    if (!mb_info_phys) return;

    mb2_info_t *info = (mb2_info_t *)(uintptr_t)mb_info_phys;
    uint8_t *ptr = (uint8_t *)(info + 1);
    uint8_t *end = (uint8_t *)info + info->total_size;

    while (ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t *)ptr;
        if (tag->type == 0) break;
        if (tag->type == MB2_TAG_MMAP) {
            mb2_tag_mmap_t *mmap = (mb2_tag_mmap_t *)tag;
            mb2_mmap_entry_t *entry_base =
                (mb2_mmap_entry_t *)((uint8_t *)mmap + sizeof(mb2_tag_mmap_t));
            uint32_t count =
                (mmap->size - sizeof(mb2_tag_mmap_t)) / mmap->entry_size;
            for (uint32_t i = 0; i < count; i++) {
                mb2_mmap_entry_t *e = &entry_base[i];
                if (e->type != MB2_MMAP_AVAILABLE) continue;
                uint64_t base = e->base_addr;
                uint64_t size = e->length;
                if (base < 0x200000) {
                    uint64_t skip = 0x200000 - base;
                    if (skip >= size) continue;
                    base += skip; size -= skip;
                }
                if (size > *out_size) { *out_base = base; *out_size = size; }
            }
        }
        ptr += MB2_ALIGN(tag->size);
    }
}


/*  Kernel heap   */

#define HEAP_SIZE (4 * 1024 * 1024)
static uint8_t g_kernel_heap[HEAP_SIZE] __attribute__((aligned(4096)));


/*  PIT 100 Hz   */

#define PIT_CMD  0x43
#define PIT_CH0  0x40
#define PIT_FREQ 1193182
#define PIT_HZ   100

uint64_t g_ticks = 0;
static void timer_irq_handler(interrupt_frame_t *frame)
{
    (void)frame;
    g_ticks++;
    if (g_current_proc) {
        extern uint64_t g_syscall_kernel_rsp;
        g_syscall_kernel_rsp = g_current_proc->kernel_stack_top;
        extern void tss_set_rsp0(uint64_t);
        tss_set_rsp0(g_current_proc->kernel_stack_top);
    }
    sched_tick();
}

static void pit_init(void)
{
    uint16_t div = (uint16_t)(PIT_FREQ / PIT_HZ);
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x36),        "Nd"((uint16_t)PIT_CMD));
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)(div & 0xFF)),"Nd"((uint16_t)PIT_CH0));
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)(div >> 8)),  "Nd"((uint16_t)PIT_CH0));
    irq_register(0, timer_irq_handler);
}


/*  Forward declarations  */

extern void keyboard_init(void);
extern void mouse_init(void);
extern void jump_to_userspace(uint64_t entry, uint64_t stack, uint64_t pml4);


/*  Shell blob symbols (objcopy)  */

extern uint8_t _binary_build_userspace_shell_exploish_elf_start[] __attribute__((weak));
extern uint8_t _binary_build_userspace_shell_exploish_elf_end[]   __attribute__((weak));

/*  Init blob symbols (objcopy)  */
extern uint8_t _binary_build_userspace_bin_init_elf_start[] __attribute__((weak));
extern uint8_t _binary_build_userspace_bin_init_elf_end[]   __attribute__((weak));


/*  kprint   */

static void kprint(const char *s)
{
    vga_puts(s);
    serial_print(s);
}


/*  kernel_main  */

void kernel_main(uint64_t mb_magic, uint64_t mb_info_phys)
{
    serial_init();
    serial_print("\n");
    serial_print("==============================================\n");
    serial_print("  EXPLOIDUS v0.1.0 -- Reactive Capability Kernel\n");
    serial_print("==============================================\n");

    vga_init();
    vga_set_color(10, 0);

    kprint("EXPLOIDUS v0.1.0 -- Reactive Capability Kernel\n");
    kprint("Copyright (c) Exploidus Project. MIT License.\n\n");

    if (mb_magic != MULTIBOOT2_MAGIC) {
        kprint("PANIC: not loaded by a Multiboot2 bootloader.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    kprint("[GDT ] Loading global descriptor table...\n");
    gdt_init();

    kprint("[IDT ] Loading interrupt descriptor table...\n");
    idt_init();

    kprint("[IRQ ] Remapping PIC (IRQ 0-15 -> vectors 32-47)...\n");
    irq_init();

    uint64_t mem_base, mem_size;
    parse_memory_map(mb_info_phys, &mem_base, &mem_size);
    parse_framebuffer(mb_info_phys);
    font_init();
    fb_console_init();

    kprint("[PMM ] Initializing colored memory zones...\n");
    pmm_init(mem_base, mem_size);

    kprint("[VMM ] Enabling paging with zone-boundary enforcement...\n");
    vmm_init();

    kprint("[HEAP] Initializing 4 MiB kernel heap...\n");
    kmalloc_init((uint64_t)(uintptr_t)g_kernel_heap, HEAP_SIZE);

    kprint("[AUD ] Starting audit ring buffer...\n");
    audit_init();

    kprint("[CAP ] Seeding capability system (BLAKE3 + RDRAND)...\n");
    cap_subsystem_init();

    kprint("[PROC] Initializing process table...\n");
    proc_init();

    kprint("[SCHED] Starting intent-based scheduler...\n");
    sched_init();

    kprint("[SYS ] Configuring SYSCALL MSR...\n");
    syscall_init();
    syscall_init_msr();

    kprint("[PIT ] Calibrating timer at 100 Hz...\n");
    pit_init();

    kprint("[ATA ] Probing disk controller...\n");
    ata_init();

    kprint("[KBD ] Registering PS/2 keyboard IRQ...\n");
    keyboard_init();

    kprint("[MOUSE] Initializing PS/2 mouse...\n");
    mouse_init();

    kprint("[VFS ] Initializing virtual filesystem layer...\n");
    vfs_init();

    kprint("[NET ] Initializing TCP/IP network stack...\n");
    net_init();
    extern void arp_preload_qemu_gateway(void);
    arp_preload_qemu_gateway();

    kprint("[IPC ] Initializing inter-process communication...\n");
    ipc_init();

    kprint("[SHM ] Initializing shared memory subsystem...\n");
    shm_init();

    kprint("[CNSL] Starting correlated security layer...\n");
    cnsl_init();

    kprint("[FIM ] Starting file integrity monitor...\n");
    fim_init();

    kprint("[HC  ] Starting HuddleCluster load balancer...\n");
    extern hc_cluster_t g_cluster;
    hc_init(&g_cluster);
    hc_add_server(&g_cluster, "exploidus-0", 0x0a00020f, 80, 1);

    kprint("[ExFS] Attempting to mount root filesystem...\n");
    vfs_node_t *fs_root = exfs_mount(0);
    if (fs_root) {
        vfs_mount("/", fs_root);
        kprint("[ExFS] Root mounted at /\n");
    } else {
        kprint("[ExFS] No ExFS volume found — continuing without disk.\n");
    }

    /* PID 1 — init */
    process_t *init_proc = proc_create(INTENT_INTERACTIVE, 0);
    if (init_proc) {
        cap_token_t fc = cap_create(CAP_RES_FILE, 0xFFFFFFFF,
            CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_EXEC |
            CAP_RIGHT_DELEGATE, init_proc->pid);
        proc_add_cap(init_proc->pid, fc, 0, true);

        cap_token_t pc = cap_create(CAP_RES_PROCESS, 0xFFFFFFFF,
            CAP_RIGHT_READ | CAP_RIGHT_SIGNAL | CAP_RIGHT_DELEGATE,
            init_proc->pid);
        proc_add_cap(init_proc->pid, pc, 0, true);
        sched_enqueue(init_proc);
        serial_print("[INIT] PID 1 created\n");
    }

    serial_print("[INIT] audit log active in kernel (auditd daemon deferred)\n");

    kprint("\nAll subsystems nominal.\n");
    kprint("Exploidus kernel ready.\n\n");

    /* Try to launch init first */
    if (_binary_build_userspace_bin_init_elf_start != NULL &&
        (void *)_binary_build_userspace_bin_init_elf_start !=
        (void *)_binary_build_userspace_bin_init_elf_end)
    {
        uint64_t init_size =
            (uint64_t)(_binary_build_userspace_bin_init_elf_end
                     - _binary_build_userspace_bin_init_elf_start);

        serial_print("[INIT] Loading init ELF\n");

        uint64_t init_pml4  = 0;
        uint64_t init_entry = 0;
        uint64_t init_stack = 0;

        if (elf_load(_binary_build_userspace_bin_init_elf_start,
                     init_size, &init_pml4, &init_entry, &init_stack,
                     (const char **)0, (const char **)0))
        {
            if (init_proc) {
                sched_dequeue(init_proc);
                init_proc->state          = PROC_RUNNING;
                init_proc->cr3            = init_pml4;
                init_proc->user_entry     = init_entry;
                init_proc->user_stack_top = init_stack;
                g_current_proc   = init_proc;
                tss_set_rsp0(init_proc->kernel_stack_top);
                g_kernel_stack_top   = init_proc->kernel_stack_top;
                g_syscall_kernel_rsp = init_proc->kernel_stack_top;
            }
            serial_print("[INIT] Jumping to init (PID 1)\n");
            __asm__ volatile("cli\njmp jump_to_userspace\n"
                : : "D"(init_entry), "S"(init_stack), "d"(init_pml4));
        }
        serial_print("[INIT] init ELF load failed, falling back to shell\n");
    }

    /* Fallback: launch shell directly */
    if (_binary_build_userspace_shell_exploish_elf_start != NULL &&
        (void *)_binary_build_userspace_shell_exploish_elf_start !=
        (void *)_binary_build_userspace_shell_exploish_elf_end)
    {
        uint64_t elf_size =
            (uint64_t)(_binary_build_userspace_shell_exploish_elf_end
                     - _binary_build_userspace_shell_exploish_elf_start);

        serial_print("[INIT] Loading exploish shell ELF (");
        serial_printhex(elf_size);
        serial_print(" bytes)\n");

        uint64_t shell_pml4  = 0;
        __asm__ volatile ("cli");
        uint64_t shell_entry = 0;
        uint64_t shell_stack = 0;

        if (elf_load(_binary_build_userspace_shell_exploish_elf_start,
                     elf_size, &shell_pml4, &shell_entry, &shell_stack,
                     (const char **)0, (const char **)0))
        {
            __asm__ volatile ("cli");

            if (init_proc) {
                sched_dequeue(init_proc);
                init_proc->state          = PROC_RUNNING;
                init_proc->cr3            = shell_pml4;
                init_proc->user_entry     = shell_entry;
                init_proc->user_stack_top = shell_stack;
                g_current_proc = init_proc;
                tss_set_rsp0(init_proc->kernel_stack_top);
                serial_print("[INIT] kernel_stack_top=");
                serial_printhex(init_proc->kernel_stack_top);
                serial_print("\n");
            }

            serial_print("[INIT] Jumping to userspace...\n");
            serial_print("[INIT] entry="); serial_printhex(shell_entry);
            serial_print(" stack=");       serial_printhex(shell_stack);
            serial_print(" pml4=");        serial_printhex(shell_pml4);
            serial_print("\n");

            __asm__ volatile("cli\njmp jump_to_userspace\n"
                : : "D"(shell_entry), "S"(shell_stack), "d"(shell_pml4));
        } else {
            kprint("[INIT] ELF load failed. Dropping to idle.\n");
        }
    } else {
        kprint("exploidus login: [shell binary not linked]\n");
        kprint("Build exploish and link with objcopy to enable userspace.\n");
        serial_print("[KERNEL] No shell binary. Idling.\n");
    }

    /* idle loop */
    __asm__ volatile ("sti");
    for (;;) {
        net_poll();
        __asm__ volatile ("hlt");
    }
}