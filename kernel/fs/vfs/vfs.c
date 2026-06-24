#include "vfs.h"
#include "../../drivers/serial.h"
#include "../../cnsl/fim.h"
#include "../../proc/process.h"
#include "../../mm/kmalloc.h"
#include <string.h>

#define MAX_MOUNTS 16

typedef struct {
    char mountpoint[VFS_NAME_MAX + 1];
    vfs_node_t *root;
    bool active;
} mount_entry_t;

typedef struct {
    vfs_node_t *node;
    uint64_t offset;
    bool active;
    uint32_t owner_pid;  /* which process opened this fd, 0 = boot/kernel context.
                          * Used so proc_exit() can reclaim leaked fds instead of
                          * permanently consuming a slot in the small (64-entry)
                          * global fd table for the lifetime of the system. */
} fd_entry_t;

static mount_entry_t g_mounts[MAX_MOUNTS];
static fd_entry_t g_fds[VFS_MAX_FDS];
static int g_mount_count = 0;
static vfs_node_t *g_cwd = NULL;  /* current working directory */

void vfs_init(void)
{
    memset(g_mounts, 0, sizeof(g_mounts));
    memset(g_fds, 0, sizeof(g_fds));
    g_mount_count = 0;

    serial_print("[VFS] initialized\n");
    g_cwd = NULL;
}

/*  SAFE MOUNT  */
int vfs_mount(const char *mountpoint, vfs_node_t *root)
{
    if (!mountpoint || !root)
        return -1;

    if (g_mount_count >= MAX_MOUNTS)
        return -1;

    mount_entry_t *m = &g_mounts[g_mount_count++];

    m->active = true;
    m->root = root;

    uint64_t len = strlen(mountpoint);
    if (len > VFS_NAME_MAX)
        len = VFS_NAME_MAX;

    memcpy(m->mountpoint, mountpoint, len);
    m->mountpoint[len] = '\0';

    serial_print("[VFS] mounted\n");
    return 0;
}

/*  SAFE LOOKUP  */
vfs_node_t *vfs_lookup(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;

    vfs_node_t *best_root = NULL;
    int best_len = -1;

    for (int i = 0; i < g_mount_count; i++) {

        if (!g_mounts[i].active)
            continue;

        int mlen = (int)strlen(g_mounts[i].mountpoint);

        if (mlen <= 0)
            continue;

        /* SAFE CHECK: ensure path is long enough */
        if ((int)strlen(path) < mlen)
            continue;

        if (memcmp(path, g_mounts[i].mountpoint, mlen) == 0) {
            if (mlen > best_len) {
                best_root = g_mounts[i].root;
                best_len = mlen;
            }
        }
    }

    if (!best_root)
        return NULL;

    if (!best_root->ops || !best_root->ops->lookup)
        return best_root;

    const char *cur = path + best_len;

    while (*cur == '/')
        cur++;

    vfs_node_t *node = best_root;

    while (*cur && node) {

        const char *slash = cur;
        while (*slash && *slash != '/')
            slash++;

        char component[VFS_NAME_MAX + 1];
        uint64_t clen = (uint64_t)(slash - cur);

        if (clen > VFS_NAME_MAX)
            return NULL;

        memcpy(component, cur, clen);
        component[clen] = '\0';

        if (!node->ops || !node->ops->lookup)
            return NULL;

        vfs_node_t *next = node->ops->lookup(node, component);

        /* `node` was only needed to find `next` — it's never the
         * persistent mount root (best_root) once we're inside this
         * loop, and nothing else holds a reference to it, so it must
         * be freed here or it leaks forever. This was previously
         * unconditional: EVERY intermediate path component leaked a
         * fresh vfs_node_t + exfs_node_data_t pair on every lookup
         * (e.g. resolving "/var/rahu/installed.json" leaked 2 nodes
         * every single time), eventually exhausting the kernel heap
         * after enough commands/lookups in a session — after which
         * kzalloc() starts failing and lookups for perfectly valid,
         * existing files start returning NULL ("not found"). */
        if (node != best_root) {
            kfree(node->fs_data);
            kfree(node);
        }

        node = next;

        cur = slash;
        while (*cur == '/')
            cur++;
    }

    return node;
}

/*  SAFE OPEN  */
int vfs_open(const char *path, uint32_t flags)
{
    vfs_node_t *node = vfs_lookup(path);
    if (!node)
        return -1;

    if (!node->ops) {
        if (node->parent != NULL) { kfree(node->fs_data); kfree(node); }
        return -1;
    }

    for (int i = 3; i < VFS_MAX_FDS; i++) {

        if (!g_fds[i].active) {

            if (node->ops->open)
                node->ops->open(node, flags);

            g_fds[i].node = node;
            g_fds[i].offset = 0;
            g_fds[i].active = true;
            g_fds[i].owner_pid = g_current_proc ? g_current_proc->pid : 0;

            return i;
        }
    }

    /* fd table full — this node would otherwise leak forever */
    if (node->parent != NULL) { kfree(node->fs_data); kfree(node); }
    return -1;
}

/*  SAFE CLOSE  */
int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
        return -1;

    if (!g_fds[fd].active)
        return -1;

    vfs_node_t *node = g_fds[fd].node;

    if (node && node->ops && node->ops->close)
        node->ops->close(node);

    /* This fd exclusively owns `node` (dup()/dup2() exist as syscalls
     * but are never actually called anywhere in this codebase, so no
     * other fd can be aliasing the same node) — free it now, unless
     * it's a mount root (parent==NULL), which is a permanent singleton
     * that must never be freed. */
    if (node && node->parent != NULL) {
        kfree(node->fs_data);
        kfree(node);
    }

    g_fds[fd].active = false;
    g_fds[fd].node = NULL;
    g_fds[fd].offset = 0;
    g_fds[fd].owner_pid = 0;

    return 0;
}

/*
 * vfs_close_all_for_pid — release every fd still owned by `pid`.
 *
 * g_fds[] is a single global table shared by the whole system (not
 * per-process), and historically nothing ever reclaimed a slot unless
 * the owning process explicitly called close()/vfs_close() itself
 * before exiting. A process that opened a file and then crashed, was
 * killed, or simply forgot to close it would permanently leak that
 * slot for the lifetime of the system — with only VFS_MAX_FDS (64)
 * slots total, a handful of such leaks across repeated spawns is
 * enough to exhaust the table, after which vfs_open() starts failing
 * for completely unrelated files/processes (observed symptom: spawn
 * reporting a perfectly valid, existing binary as "not found", because
 * vfs_open() inside sys_spawn returned -1 due to a full table, not
 * because the directory entry was actually missing).
 *
 * Called from proc_exit() so PID 0 (no process / boot-time context)
 * is never swept, since g_next_pid starts at 1 and 0 is never a real
 * process's id.
 */
void vfs_close_all_for_pid(uint32_t pid)
{
    if (!pid) return;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (g_fds[i].active && g_fds[i].owner_pid == pid) {
            vfs_close(i);
        }
    }
}

/*  SAFE READ  */
int64_t vfs_read(int fd, void *buf, uint64_t len)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active)
        return -1;

    vfs_node_t *node = g_fds[fd].node;

    if (!node || !node->ops || !node->ops->read)
        return -1;

    int64_t n = node->ops->read(node, g_fds[fd].offset, buf, len);

    if (n > 0)
        g_fds[fd].offset += (uint64_t)n;

    return n;
}

/*  SAFE WRITE  */
int64_t vfs_write(int fd, const void *buf, uint64_t len)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active)
        return -1;

    vfs_node_t *node = g_fds[fd].node;

    if (!node || !node->ops || !node->ops->write)
        return -1;

    /* FIM: check if this is a critical path before writing */
    if (node->name[0])
        fim_on_write(node->name);

    int64_t n = node->ops->write(node, g_fds[fd].offset, buf, len);

    if (n > 0)
        g_fds[fd].offset += (uint64_t)n;

    return n;
}
int64_t vfs_readdir(int fd, void *buf, uint64_t max)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active) return -1;
    vfs_node_t *node = g_fds[fd].node;
    if (!node || node->type != VFS_DIRECTORY) return -1;
    if (!node->ops || !node->ops->readdir) return -1;
    int64_t n = node->ops->readdir(node, g_fds[fd].offset, buf, max);
    if (n > 0)
        g_fds[fd].offset += (uint64_t)n;
    return n;
}

int vfs_create(const char *path, uint8_t type)
{
    /* Split path into parent dir + filename */
    char parent[256]; int plen = 0;
    const char *p = path;
    const char *last_slash = path;
    while (*p) { if (*p == '/') last_slash = p; p++; }
    const char *name = (*last_slash == '/') ? last_slash + 1 : path;
    if (!*name) return -1;
    if (strlen(name) > VFS_NAME_MAX) return -1;

    if (last_slash == path) {
        parent[0] = '/'; parent[1] = '\0';
    } else {
        plen = (int)(last_slash - path);
        if (plen >= 255) return -1;
        for (int i = 0; i < plen; i++) parent[i] = path[i];
        parent[plen] = '\0';
    }

    vfs_node_t *dir = vfs_lookup(parent);
    if (!dir || dir->type != VFS_DIRECTORY) return -1;
    if (!dir->ops || !dir->ops->create) return -1;

    vfs_node_t *newnode = dir->ops->create(dir, name, type);

    /* `dir` was only needed to call ->create() — free it now unless
     * it's a mount root (roots have parent==NULL and are permanent,
     * shared singletons; every other node here is a fresh allocation
     * from this exact vfs_lookup() call that nothing else references). */
    if (dir->parent != NULL) {
        kfree(dir->fs_data);
        kfree(dir);
    }

    if (!newnode) return -1;

    /* FIM: check if new file is in a watched path */
    fim_on_create(path);

    /* Open the new file and return fd */
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!g_fds[i].active) {
            if (newnode->ops && newnode->ops->open)
                newnode->ops->open(newnode, 0);
            g_fds[i].node      = newnode;
            g_fds[i].offset    = 0;
            g_fds[i].active    = true;
            g_fds[i].owner_pid = g_current_proc ? g_current_proc->pid : 0;
            return i;
        }
    }
    /* fd table full — newnode is always a fresh child node here
     * (never a mount root), safe to free unconditionally */
    kfree(newnode->fs_data);
    kfree(newnode);
    return -1;
}

vfs_node_t *vfs_get_cwd(void)
{
    return g_cwd;
}

int vfs_unlink(const char *path)
{
    /* Split path into parent dir + filename — same approach as vfs_create */
    char parent[256]; int plen = 0;
    const char *p = path;
    const char *last_slash = path;
    while (*p) { if (*p == '/') last_slash = p; p++; }
    const char *name = (*last_slash == '/') ? last_slash + 1 : path;
    if (!*name) return -1;
    if (strlen(name) > VFS_NAME_MAX) return -1;

    if (last_slash == path) {
        parent[0] = '/'; parent[1] = '\0';
    } else {
        plen = (int)(last_slash - path);
        if (plen >= 255) return -1;
        for (int i = 0; i < plen; i++) parent[i] = path[i];
        parent[plen] = '\0';
    }

    vfs_node_t *dir = vfs_lookup(parent);
    if (!dir || dir->type != VFS_DIRECTORY) return -1;
    if (!dir->ops || !dir->ops->unlink) {
        if (dir->parent != NULL) { kfree(dir->fs_data); kfree(dir); }
        return -1;
    }

    int result = dir->ops->unlink(dir, name);

    /* `dir` was only needed to call ->unlink() — free it now unless
     * it's a mount root (see vfs_create for the same pattern/reasoning). */
    if (dir->parent != NULL) {
        kfree(dir->fs_data);
        kfree(dir);
    }

    return result;
}

int vfs_chdir(const char *path)
{
    static char abspath[512];

    /* Convert relative path to absolute */
    if (path[0] != '/') {
        /* build absolute path from cwd */
        if (g_cwd && g_cwd->name[0]) {
            /* Try /mountpoint/path */
            abspath[0] = '/';
            abspath[1] = '\0';
            /* find cwd path by checking mounts */
            for (int i = 0; i < g_mount_count; i++) {
                if (!g_mounts[i].active) continue;

                bool mount_matches = (g_mounts[i].root == g_cwd);
                if (!mount_matches) {
                    /* This was previously just `... ->lookup(root, path)`
                     * used as a truthy check, discarding the returned
                     * node every time — leaking one node per mount on
                     * every single relative cd. Free it immediately
                     * since it's only needed as an existence probe. */
                    vfs_node_t *probe = g_mounts[i].root->ops->lookup(g_mounts[i].root, path);
                    if (probe) {
                        mount_matches = true;
                        if (probe->parent != NULL) { kfree(probe->fs_data); kfree(probe); }
                    }
                }

                if (mount_matches) {
                    /* build /mountpoint/path */
                    uint64_t mlen = strlen(g_mounts[i].mountpoint);
                    uint64_t plen = strlen(path);
                    if (mlen + plen + 2 < 512) {
                        memcpy(abspath, g_mounts[i].mountpoint, mlen);
                        if (abspath[mlen-1] != '/') abspath[mlen++] = '/';
                        memcpy(abspath + mlen, path, plen + 1);
                        vfs_node_t *node = vfs_lookup(abspath);
                        if (node && node->type == VFS_DIRECTORY) {
                            if (g_cwd && g_cwd->parent != NULL) {
                                kfree(g_cwd->fs_data);
                                kfree(g_cwd);
                            }
                            g_cwd = node;
                            return 0;
                        }
                        if (node && node->parent != NULL) {
                            kfree(node->fs_data);
                            kfree(node);
                        }
                    }
                }
            }
        }
        /* fallback: try /path */
        abspath[0] = '/';
        uint64_t plen = strlen(path);
        if (plen + 2 < 512) {
            memcpy(abspath + 1, path, plen + 1);
            path = abspath;
        }
    }

    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -1;
    if (node->type != VFS_DIRECTORY) {
        if (node->parent != NULL) { kfree(node->fs_data); kfree(node); }
        return -1;
    }
    if (g_cwd && g_cwd->parent != NULL) {
        kfree(g_cwd->fs_data);
        kfree(g_cwd);
    }
    g_cwd = node;
    return 0;
}

int vfs_getcwd(char *buf, uint64_t size)
{
    if (!buf || size == 0) return -1;
    if (!g_cwd) {
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }
    /* build path by walking parent chain */
    static char tmp[1024];
    int pos = 0;
    vfs_node_t *node = g_cwd;
    /* collect components */
    static char parts[16][VFS_NAME_MAX+1];
    int depth = 0;
    while (node && node->parent && node != node->parent && depth < 16) {
        int i = 0;
        while (node->name[i] && i < VFS_NAME_MAX) {
            parts[depth][i] = node->name[i];
            i++;
        }
        parts[depth][i] = '\0';
        depth++;
        node = node->parent;
    }
    tmp[pos++] = '/';
    for (int i = depth-1; i >= 0; i--) {
        int j = 0;
        while (parts[i][j]) tmp[pos++] = parts[i][j++];
        if (i > 0) tmp[pos++] = '/';
    }
    tmp[pos] = '\0';
    uint64_t len = (uint64_t)pos;
    if (len >= size) len = size-1;
    for (uint64_t i = 0; i < len; i++) buf[i] = tmp[i];
    buf[len] = '\0';
    return 0;
}

/* SEEK constants (matching Linux/POSIX) */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

int64_t vfs_lseek(int fd, int64_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active) return -1;
    vfs_node_t *node = g_fds[fd].node;
    if (!node) return -1;

    int64_t new_offset;
    switch (whence) {
    case VFS_SEEK_SET:
        new_offset = offset;
        break;
    case VFS_SEEK_CUR:
        new_offset = (int64_t)g_fds[fd].offset + offset;
        break;
    case VFS_SEEK_END:
        new_offset = (int64_t)node->size + offset;
        break;
    default:
        return -1;
    }

    if (new_offset < 0) return -1;
    g_fds[fd].offset = (uint64_t)new_offset;
    return new_offset;
}

int vfs_fstat(int fd, vfs_stat_t *st)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active) return -1;
    if (!st) return -1;
    vfs_node_t *node = g_fds[fd].node;
    if (!node) return -1;
    st->size  = node->size;
    st->type  = (uint32_t)node->type;
    st->inode = node->inode;
    return 0;
}

int vfs_stat(const char *path, vfs_stat_t *st)
{
    if (!path || !st) return -1;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -1;
    st->size  = node->size;
    st->type  = (uint32_t)node->type;
    st->inode = node->inode;
    if (node->parent != NULL) { kfree(node->fs_data); kfree(node); }
    return 0;
}

/*
 * WARNING: vfs_close() now frees the fd's vfs_node_t when it's closed
 * (see vfs_close — this was needed to fix a kernel-heap leak where
 * every lookup/open permanently leaked its node). vfs_dup()/vfs_dup2()
 * copy the fd entry, including the raw `node` pointer, so the
 * duplicate fd ends up ALIASING the same node as the original. If
 * EITHER fd is closed, the node is freed — and the other fd is left
 * pointing at freed memory (use-after-free) the next time it's used.
 *
 * As of this fix, dup()/dup2() are not called anywhere in this
 * codebase's userspace code, so this is currently a dormant risk, not
 * an active bug. If dup/dup2 usage is added later, this needs proper
 * reference counting on vfs_node_t before it's safe.
 */
int vfs_dup(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_fds[fd].active) return -1;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!g_fds[i].active) {
            g_fds[i] = g_fds[fd];   /* copy node + offset */
            return i;
        }
    }
    return -1;
}

int vfs_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !g_fds[oldfd].active) return -1;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -1;
    if (newfd == oldfd) return newfd;
    if (g_fds[newfd].active) vfs_close(newfd);
    g_fds[newfd] = g_fds[oldfd];
    return newfd;
}