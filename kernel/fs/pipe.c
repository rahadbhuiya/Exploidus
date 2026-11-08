#include "pipe.h"
#include "../mm/kmalloc.h"
#include "../proc/scheduler.h"
#include <string.h>

extern int vfs_bind_node_to_fd(vfs_node_t *node);

typedef struct {
    pipe_t *pipe;
    bool    is_write;
} pipe_endpoint_t;

static int64_t pipe_op_read(vfs_node_t *node, uint64_t offset, void *buf, uint64_t len)
{
    (void)offset;
    if (!node || !node->fs_data || !buf || len == 0) return 0;
    pipe_endpoint_t *ep = (pipe_endpoint_t *)node->fs_data;
    pipe_t *p = ep->pipe;
    if (!p) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t read_bytes = 0;

    while (read_bytes < len) {
        if (p->count == 0) {
            if (!p->write_open) {
                break; /* EOF */
            }
            if (read_bytes > 0) {
                break; /* Return data collected so far */
            }
            sched_yield(); /* Wait for writer to produce data */
            if (!p->write_open && p->count == 0) break;
            continue;
        }

        dst[read_bytes++] = p->buffer[p->read_idx];
        p->read_idx = (p->read_idx + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    return (int64_t)read_bytes;
}

static int64_t pipe_op_write(vfs_node_t *node, uint64_t offset, const void *buf, uint64_t len)
{
    (void)offset;
    if (!node || !node->fs_data || !buf || len == 0) return 0;
    pipe_endpoint_t *ep = (pipe_endpoint_t *)node->fs_data;
    pipe_t *p = ep->pipe;
    if (!p) return -1;

    if (!p->read_open) {
        return -1; /* Broken pipe / EPIPE */
    }

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t written = 0;

    while (written < len) {
        if (p->count >= PIPE_BUF_SIZE) {
            if (!p->read_open) return -1;
            sched_yield(); /* Wait for reader to consume */
            continue;
        }

        p->buffer[p->write_idx] = src[written++];
        p->write_idx = (p->write_idx + 1) % PIPE_BUF_SIZE;
        p->count++;
    }

    return (int64_t)written;
}

static int pipe_op_close(vfs_node_t *node)
{
    if (!node || !node->fs_data) return 0;
    pipe_endpoint_t *ep = (pipe_endpoint_t *)node->fs_data;
    pipe_t *p = ep->pipe;

    if (p) {
        if (ep->is_write) {
            p->write_open = false;
        } else {
            p->read_open = false;
        }

        if (!p->read_open && !p->write_open) {
            kfree(p);
            ep->pipe = NULL;
        }
    }

    return 0;
}

static const vfs_ops_t g_pipe_ops = {
    .read  = pipe_op_read,
    .write = pipe_op_write,
    .close = pipe_op_close,
};

int pipe_create(int fds[2])
{
    if (!fds) return -1;

    pipe_t *p = kmalloc(sizeof(pipe_t));
    if (!p) return -1;
    memset(p, 0, sizeof(pipe_t));
    p->read_open = true;
    p->write_open = true;

    /* Create read endpoint node */
    vfs_node_t *r_node = kmalloc(sizeof(vfs_node_t));
    pipe_endpoint_t *r_ep = kmalloc(sizeof(pipe_endpoint_t));
    if (!r_node || !r_ep) {
        if (r_node) kfree(r_node);
        if (r_ep) kfree(r_ep);
        kfree(p);
        return -1;
    }
    memset(r_node, 0, sizeof(vfs_node_t));
    r_ep->pipe = p;
    r_ep->is_write = false;
    r_node->type = VFS_FILE;
    r_node->ops = &g_pipe_ops;
    r_node->fs_data = r_ep;
    r_node->refcount = 1;

    /* Create write endpoint node */
    vfs_node_t *w_node = kmalloc(sizeof(vfs_node_t));
    pipe_endpoint_t *w_ep = kmalloc(sizeof(pipe_endpoint_t));
    if (!w_node || !w_ep) {
        kfree(r_node);
        kfree(r_ep);
        if (w_node) kfree(w_node);
        if (w_ep) kfree(w_ep);
        kfree(p);
        return -1;
    }
    memset(w_node, 0, sizeof(vfs_node_t));
    w_ep->pipe = p;
    w_ep->is_write = true;
    w_node->type = VFS_FILE;
    w_node->ops = &g_pipe_ops;
    w_node->fs_data = w_ep;
    w_node->refcount = 1;

    int rfd = vfs_bind_node_to_fd(r_node);
    int wfd = vfs_bind_node_to_fd(w_node);

    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) vfs_close(rfd);
        if (wfd >= 0) vfs_close(wfd);
        kfree(r_ep); kfree(r_node);
        kfree(w_ep); kfree(w_node);
        kfree(p);
        return -1;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}
