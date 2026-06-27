/*
 * ipc.h — Exploidus Inter-Process Communication
 *
 * Simple synchronous message-passing.
 * Each process has a small fixed-size inbox (IPC_QUEUE_DEPTH messages).
 *
 * Design rules:
 *   - No dynamic allocation (no kmalloc): the queue is embedded in ipc_state_t
 *     which is allocated once per process at proc_create() time.
 *   - Messages are fixed-size (IPC_MSG_PAYLOAD bytes max).
 *   - ipc_send() copies the message into the receiver's inbox and
 *     unblocks it if it was waiting.
 *   - ipc_recv() blocks (yields) until a message arrives.
 *   - Both sides reference processes by PID — no handles.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/*  tunables  */
#define IPC_QUEUE_DEPTH  8          /* messages buffered per process      */
#define IPC_MSG_PAYLOAD  120        /* bytes of user payload per message  */

/*  message struct  */
typedef struct {
    uint32_t from_pid;              /* sender PID (filled by kernel)      */
    uint32_t type;                  /* caller-defined message type        */
    uint8_t  data[IPC_MSG_PAYLOAD]; /* raw payload                        */
    uint32_t len;                   /* bytes valid in data[]              */
} ipc_msg_t;                        /* 132 bytes                          */

/*  per-process IPC queue  */
typedef struct {
    ipc_msg_t queue[IPC_QUEUE_DEPTH];
    uint8_t   head;                 /* dequeue index                      */
    uint8_t   tail;                 /* enqueue index                      */
    uint8_t   count;                /* messages currently queued          */
    uint8_t   _pad;
    bool      waiting;              /* true when process is blocked on recv */
} ipc_state_t;

/*  subsystem API  */
void ipc_init(void);

/*
 * ipc_alloc_state — called by proc_create() to give each new process
 *                   its own ipc_state_t.  Returns pointer to allocated
 *                   state, or NULL on OOM.  The caller stores this in
 *                   process_t.ipc.
 */
ipc_state_t *ipc_alloc_state(void);

/*
 * ipc_free_state — called by proc_exit() to release the state.
 */
void ipc_free_state(ipc_state_t *s);

/*
 * ipc_send — copy msg into dst_pid's inbox.
 *   Returns  0  on success.
 *   Returns -1  if dst_pid doesn't exist.
 *   Returns -2  if dst_pid's inbox is full.
 *   Wakes dst_pid if it is blocked in ipc_recv().
 */
int ipc_send(uint32_t dst_pid, const ipc_msg_t *msg);

/*
 * ipc_recv — copy one message from the current process's inbox into out.
 *   Blocks (yields) if inbox is empty.
 *   Returns 0 on success.
 *   Returns -1 if current process has no IPC state.
 */
int ipc_recv(ipc_msg_t *out);

/*
 * ipc_recv_nb — non-blocking variant.
 *   Returns 0 if a message was dequeued, -1 if empty.
 */
int ipc_recv_nb(ipc_msg_t *out);