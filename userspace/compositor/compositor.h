/*
 * compositor.h — Exploidus Compositor Protocol
 *
 * Shared between /bin/compositor and every GUI app.
 * Apps include this header, use ipc_send() to register windows,
 * and draw into their SHM pixel buffer. The compositor blends
 * all buffers onto the physical framebuffer each frame.
 *
 * Protocol flow:
 *   1. App calls shm_create(w * h * 4)         → shm_id
 *   2. App calls shm_map(shm_id)               → pixel buffer VA
 *   3. App sends IPC_MSG_WIN_CREATE to COMPOSITOR_PID
 *   4. Compositor maps the same shm_id         → reads pixels
 *   5. App draws into its buffer, sends IPC_MSG_DAMAGE
 *   6. Compositor blends on next frame tick
 *   7. On close: IPC_MSG_WIN_DESTROY, shm_destroy
 */

#pragma once

#ifdef __EXPLOIDUS_USERSPACE__
#  include "../libc/syscall.h"
#else
#  include <stdint.h>
#endif

/*  Compositor PID lookup  */
/*
 * compositor_pid() — read /tmp/compositor.pid at runtime.
 * Returns the compositor's PID, or 0 if not found.
 * Apps should call this once at startup and cache the result.
 */
#ifdef __EXPLOIDUS_USERSPACE__
static inline uint32_t compositor_pid(void)
{
    int fd = open("/tmp/compositor.pid", O_RDONLY);
    if (fd < 0) return 0;
    char buf[16]; int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    uint32_t pid = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
        pid = pid * 10 + (uint32_t)(buf[i] - '0');
    return pid;
}
#endif

/*  Window flags  */
#define WIN_FLAG_NONE       0x00
#define WIN_FLAG_DECORATED  0x01   /* compositor draws title bar        */
#define WIN_FLAG_TOPMOST    0x02   /* always on top                     */
#define WIN_FLAG_NO_RESIZE  0x04   /* fixed size                        */

/*  IPC message payloads  */
/*
 * All payloads fit in ipc_msg_t.data[120].
 * msg.type is one of the IPC_MSG_WIN_* constants from syscall.h.
 */

/* IPC_MSG_WIN_CREATE  (app → compositor) */
typedef struct __attribute__((packed)) {
    uint32_t shm_id;       /* SHM region containing ARGB pixels         */
    int32_t  x, y;         /* initial position                          */
    uint32_t w, h;         /* pixel dimensions (must match SHM size)    */
    uint32_t flags;        /* WIN_FLAG_* bitmask                        */
    char     title[48];    /* window title (null-terminated)            */
} win_create_msg_t;        /* 72 bytes — fits in payload[120]           */

/* IPC_MSG_WIN_DESTROY (app → compositor) */
typedef struct __attribute__((packed)) {
    uint32_t shm_id;       /* which window to remove                    */
} win_destroy_msg_t;

/* IPC_MSG_WIN_MOVE    (compositor/WM → compositor internal) */
typedef struct __attribute__((packed)) {
    uint32_t shm_id;
    int32_t  new_x, new_y;
} win_move_msg_t;

/* IPC_MSG_DAMAGE      (app → compositor) */
typedef struct __attribute__((packed)) {
    uint32_t shm_id;       /* which window has new pixels               */
    int32_t  dx, dy;       /* dirty region (0,0,w,h = full repaint)    */
    uint32_t dw, dh;
} win_damage_msg_t;

/* IPC_MSG_MOUSE_BTN   (compositor → app) */
typedef struct __attribute__((packed)) {
    int32_t  x, y;         /* position relative to window origin        */
    uint8_t  button;       /* 1=left 2=right 4=middle                   */
    uint8_t  pressed;      /* 1=down 0=up                               */
} mouse_btn_msg_t;

/* IPC_MSG_MOUSE_MOVE  (compositor → app) */
typedef struct __attribute__((packed)) {
    int32_t  x, y;         /* position relative to window origin        */
} mouse_move_msg_t;

/* IPC_MSG_KEY_DOWN / KEY_UP (compositor → app) */
typedef struct __attribute__((packed)) {
    uint8_t  scancode;
    uint8_t  ascii;
} key_msg_t;

/*  Helper: send a win_create to the compositor  */
#ifdef __EXPLOIDUS_USERSPACE__
static inline int comp_win_create(uint32_t comp_pid,
                                   uint32_t shm_id,
                                   int32_t x, int32_t y,
                                   uint32_t w, uint32_t h,
                                   uint32_t flags,
                                   const char *title)
{
    ipc_msg_t msg;
    msg.type = IPC_MSG_WIN_CREATE;
    msg.len  = sizeof(win_create_msg_t);

    win_create_msg_t *p = (win_create_msg_t *)msg.data;
    p->shm_id = shm_id;
    p->x = x; p->y = y;
    p->w = w; p->h = h;
    p->flags = flags;
    int i = 0;
    while (title && title[i] && i < 47) { p->title[i] = title[i]; i++; }
    p->title[i] = 0;

    return (int)ipc_send(comp_pid, &msg);
}

static inline int comp_win_destroy(uint32_t comp_pid, uint32_t shm_id)
{
    ipc_msg_t msg;
    msg.type = IPC_MSG_WIN_DESTROY;
    msg.len  = sizeof(win_destroy_msg_t);
    ((win_destroy_msg_t *)msg.data)->shm_id = shm_id;
    return (int)ipc_send(comp_pid, &msg);
}

static inline int comp_damage(uint32_t comp_pid,
                               uint32_t shm_id,
                               int32_t dx, int32_t dy,
                               uint32_t dw, uint32_t dh)
{
    ipc_msg_t msg;
    msg.type = IPC_MSG_DAMAGE;
    msg.len  = sizeof(win_damage_msg_t);
    win_damage_msg_t *p = (win_damage_msg_t *)msg.data;
    p->shm_id = shm_id;
    p->dx = dx; p->dy = dy; p->dw = dw; p->dh = dh;
    return (int)ipc_send(comp_pid, &msg);
}
#endif /* __EXPLOIDUS_USERSPACE__ */