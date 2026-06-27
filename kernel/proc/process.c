#include "process.h"
#include "../mm/pmm.h"
#include "../mm/kmalloc.h"
#include "../audit/audit.h"
#include "../drivers/serial.h"
#include "../fs/vfs/vfs.h"
#include "../ipc/ipc.h"
#include <string.h>

static process_t g_proc_table[MAX_PROCESSES];
static uint32_t  g_next_pid = 1;

process_t *g_current_proc     = NULL;
uint64_t   g_kernel_stack_top = 0;   /* updated by scheduler on every context switch */

/*  SAFE ENTRY POINT  */
static void proc_idle_loop(void)
{
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void proc_init(void)
{
    memset(g_proc_table, 0, sizeof(g_proc_table));

    for (int i = 0; i < MAX_PROCESSES; i++) {
        g_proc_table[i].state = PROC_UNUSED;
    }

    g_next_pid = 1;

    serial_print("[PROC] initialized\n");
}

process_t *proc_create(proc_intent_t intent, uint32_t parent_pid)
{
    process_t *p = NULL;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc_table[i].state == PROC_UNUSED) {
            p = &g_proc_table[i];
            break;
        }
    }

    if (!p) return NULL;

    memset(p, 0, sizeof(process_t));

    p->pid        = g_next_pid++;
    p->parent_pid = parent_pid;
    p->state      = PROC_BLOCKED;   /* NOT ready until sys_spawn finishes setup */
    p->intent     = intent;

    /*  KERNEL STACK  */
    p->kernel_stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!p->kernel_stack)
        return NULL;

    p->kernel_stack_top =
        (uint64_t)(uintptr_t)(p->kernel_stack + KERNEL_STACK_SIZE);

    /*  CR3  */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(p->cr3));

    /*  CONTEXT SAFE INIT  */
    memset(&p->context, 0, sizeof(cpu_context_t));

    /* CRITICAL: RIP must never be 0 */
    p->context.rip = (uint64_t)(uintptr_t)proc_idle_loop;
    p->context.rsp = p->kernel_stack_top;

    /*  MMAP REGION  */
    /*
     * User stack sits at USER_STACK_TOP = 0x00007FFFFFFFE000 (elf.c).
     * Give mmap its own VA region well below that so they never collide.
     */
    p->mmap_top = 0x00007FF000000000ULL;

    p->cap_count = 0;

    /* Allocate IPC inbox for this process */
    p->ipc = ipc_alloc_state();
    /* Non-fatal if OOM — process will just have no IPC capability */

    audit_record(AUDIT_PROC_FORK, p->pid, parent_pid, intent);

    serial_print("[PROC] created PID\n");

    return p;
}

process_t *proc_get(uint32_t pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc_table[i].pid == pid &&
            g_proc_table[i].state != PROC_UNUSED)
            return &g_proc_table[i];
    }
    return NULL;
}

void proc_exit(uint32_t pid, int code)
{
    process_t *p = proc_get(pid);
    if (!p) return;

    p->state     = PROC_ZOMBIE;
    p->exit_code = code;

    /* Release IPC inbox */
    if (p->ipc) {
        ipc_free_state(p->ipc);
        p->ipc = NULL;
    }

    /* Reclaim any fds this process never closed — otherwise the
     * global 64-slot vfs fd table leaks one slot per forgotten
     * open(), eventually causing vfs_open() to fail system-wide
     * (including for completely unrelated, perfectly valid files). */
    vfs_close_all_for_pid(pid);

    audit_record(AUDIT_PROC_EXIT, pid, (uint64_t)code, 0);

    /* Immediately unblock any process waiting on this pid */
    extern void sched_enqueue(process_t *);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_proc_table[i].state == PROC_BLOCKED &&
            g_proc_table[i].blocked_on_pid == pid) {
            g_proc_table[i].blocked_on_pid = 0;
            g_proc_table[i].state          = PROC_READY;
            sched_enqueue(&g_proc_table[i]);
        }
    }
}

/*  LINK FIX  */
const process_t *proc_get_table(uint32_t *count_out)
{
    if (count_out)
        *count_out = MAX_PROCESSES;

    return g_proc_table;
}

/*  CAPABILITY STUB  */
bool proc_add_cap(uint32_t pid, cap_token_t token,
                  uint32_t owner_pid, bool delegatable)
{
    process_t *p = proc_get(pid);
    if (!p) return false;

    if (p->cap_count >= MAX_CAPS_PER_PROC)
        return false;

    cap_entry_t *e = &p->caps[p->cap_count++];

    e->token       = token;
    e->owner_pid   = owner_pid;
    e->delegatable = delegatable;
    e->revoked     = false;
    e->expire_tick = 0;

    return true;
}