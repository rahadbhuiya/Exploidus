/*
 * ipc.c — Exploidus IPC subsystem
 *
 * Interrupt-safe: all queue mutations happen with interrupts disabled
 * (cli/sti) so they're safe from IRQ context too.
 */

#include "ipc.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../mm/kmalloc.h"
#include "../drivers/serial.h"
#include <string.h>

/* Forward declaration — process.h already includes ipc.h so this is safe */
extern process_t *proc_get(uint32_t pid);

/*  init  */

void ipc_init(void)
{
    serial_print("[IPC ] IPC subsystem ready\n");
}

/*  per-process state lifecycle  */

ipc_state_t *ipc_alloc_state(void)
{
    ipc_state_t *s = (ipc_state_t *)kmalloc(sizeof(ipc_state_t));
    if (!s) return NULL;
    /* zero the whole struct: head=tail=count=0, waiting=false */
    uint8_t *p = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof(ipc_state_t); i++) p[i] = 0;
    return s;
}

void ipc_free_state(ipc_state_t *s)
{
    if (s) kfree(s);
}

/*  send  */

int ipc_send(uint32_t dst_pid, const ipc_msg_t *msg)
{
    if (!msg) return -1;

    process_t *dst = proc_get(dst_pid);
    if (!dst || dst->state == PROC_UNUSED || dst->state == PROC_ZOMBIE)
        return -1;

    ipc_state_t *s = dst->ipc;
    if (!s) return -1;

    /* Disable interrupts while touching the queue */
    __asm__ volatile ("cli" ::: "memory");

    if (s->count >= IPC_QUEUE_DEPTH) {
        __asm__ volatile ("sti" ::: "memory");
        return -2;  /* inbox full */
    }

    /* Copy message into tail slot */
    ipc_msg_t *slot = &s->queue[s->tail];
    uint32_t copy_len = msg->len;
    if (copy_len > IPC_MSG_PAYLOAD) copy_len = IPC_MSG_PAYLOAD;

    slot->from_pid = msg->from_pid;
    slot->type     = msg->type;
    slot->len      = copy_len;
    for (uint32_t i = 0; i < copy_len; i++)
        slot->data[i] = msg->data[i];

    s->tail  = (uint8_t)((s->tail + 1) % IPC_QUEUE_DEPTH);
    s->count++;

    /* Wake receiver if it was blocked */
    bool was_waiting = s->waiting;
    if (was_waiting) {
        s->waiting  = false;
        dst->state  = PROC_READY;
        sched_enqueue(dst);
    }

    __asm__ volatile ("sti" ::: "memory");

    /* NOTE: previously logged "[IPC ] woke PID ..." here on every
     * message that woke a blocked receiver — that's every keystroke
     * and every routed window event. Each serial_print busy-waits on
     * real UART timing, so this was adding measurable per-keystroke
     * latency system-wide. */

    return 0;
}

/*  recv (blocking)  */

int ipc_recv(ipc_msg_t *out)
{
    if (!out) return -1;

    extern process_t *g_current_proc;
    process_t *self = g_current_proc;
    if (!self) return -1;

    ipc_state_t *s = self->ipc;
    if (!s) return -1;

    /* Spin-yield until a message arrives */
    while (1) {
        __asm__ volatile ("cli" ::: "memory");

        if (s->count > 0) {
            /* Dequeue from head */
            *out = s->queue[s->head];
            s->head  = (uint8_t)((s->head + 1) % IPC_QUEUE_DEPTH);
            s->count--;
            __asm__ volatile ("sti" ::: "memory");
            return 0;
        }

        /* Queue empty — block and yield */
        s->waiting  = true;
        self->state = PROC_BLOCKED;
        __asm__ volatile ("sti" ::: "memory");
        sched_yield();
        /* We get here after ipc_send() re-enqueued us */
    }
}

/*  recv (non-blocking)  */

int ipc_recv_nb(ipc_msg_t *out)
{
    if (!out) return -1;

    extern process_t *g_current_proc;
    process_t *self = g_current_proc;
    if (!self) return -1;

    ipc_state_t *s = self->ipc;
    if (!s) return -1;

    __asm__ volatile ("cli" ::: "memory");

    if (s->count == 0) {
        __asm__ volatile ("sti" ::: "memory");
        return -1;  /* empty */
    }

    *out = s->queue[s->head];
    s->head  = (uint8_t)((s->head + 1) % IPC_QUEUE_DEPTH);
    s->count--;

    __asm__ volatile ("sti" ::: "memory");
    return 0;
}