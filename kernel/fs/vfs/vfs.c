#include "vfs.h"
#include "../../drivers/serial.h"
#include "../../cnsl/fim.h"
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

        node = node->ops->lookup(node, component);

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

    if (!node->ops)
        return -1;

    for (int i = 3; i < VFS_MAX_FDS; i++) {

        if (!g_fds[i].active) {

            if (node->ops->open)
                node->ops->open(node, flags);

            g_fds[i].node = node;
            g_fds[i].offset = 0;
            g_fds[i].active = true;

            return i;
        }
    }

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

    g_fds[fd].active = false;
    g_fds[fd].node = NULL;
    g_fds[fd].offset = 0;

    return 0;
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
    if (!newnode) return -1;

    /* FIM: check if new file is in a watched path */
    fim_on_create(path);

    /* Open the new file and return fd */
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!g_fds[i].active) {
            if (newnode->ops && newnode->ops->open)
                newnode->ops->open(newnode, 0);
            g_fds[i].node   = newnode;
            g_fds[i].offset = 0;
            g_fds[i].active = true;
            return i;
        }
    }
    return -1;
}

vfs_node_t *vfs_get_cwd(void)
{
    return g_cwd;
}

int vfs_chdir(const char *path)
{
    char abspath[512];

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
                if (g_mounts[i].root == g_cwd ||
                    g_mounts[i].root->ops->lookup(g_mounts[i].root, path)) {
                    /* build /mountpoint/path */
                    uint64_t mlen = strlen(g_mounts[i].mountpoint);
                    uint64_t plen = strlen(path);
                    if (mlen + plen + 2 < 512) {
                        memcpy(abspath, g_mounts[i].mountpoint, mlen);
                        if (abspath[mlen-1] != '/') abspath[mlen++] = '/';
                        memcpy(abspath + mlen, path, plen + 1);
                        vfs_node_t *node = vfs_lookup(abspath);
                        if (node && node->type == VFS_DIRECTORY) {
                            g_cwd = node;
                            return 0;
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
    if (node->type != VFS_DIRECTORY) return -1;
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
    char tmp[1024];
    int pos = 0;
    vfs_node_t *node = g_cwd;
    /* collect components */
    char parts[16][VFS_NAME_MAX+1];
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
    return 0;
}

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