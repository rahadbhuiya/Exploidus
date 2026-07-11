#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VFS_NAME_MAX  255
#define VFS_MAX_FDS   64

typedef enum {
    VFS_FILE      = 0,
    VFS_DIRECTORY = 1,
    VFS_DEVICE    = 2,
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef struct {
    int      (*open) (vfs_node_t *node, uint32_t flags);
    int      (*close)(vfs_node_t *node);
    int64_t  (*read) (vfs_node_t *node, uint64_t offset, void *buf, uint64_t len);
    int64_t  (*write)(vfs_node_t *node, uint64_t offset, const void *buf, uint64_t len);
    vfs_node_t *(*lookup) (vfs_node_t *dir, const char *name);
    int64_t    (*readdir)(vfs_node_t *dir, uint64_t offset, void *buf, uint64_t max);
    vfs_node_t *(*create) (vfs_node_t *dir, const char *name, uint8_t type);
    int      (*unlink)(vfs_node_t *dir, const char *name);
    int      (*chmod) (vfs_node_t *node, uint32_t mode); /* persist mode to disk; NULL = unsupported */
} vfs_ops_t;

struct vfs_node {
    char            name[VFS_NAME_MAX + 1];
    vfs_node_type_t type;
    uint64_t        size;
    uint64_t        inode;
    const vfs_ops_t *ops;
    void            *fs_data;  /* filesystem-private pointer */
    vfs_node_t      *parent;

    /* Unix-style permission bits (owner/group/other rwx), populated by
     * whichever filesystem created this node. Default 0777 (fully
     * permissive) for filesystems/pseudo-nodes that don't track modes,
     * so this is purely additive — nothing that worked before is
     * newly restricted unless a filesystem actually sets a tighter
     * value (ExFS does, from its on-disk inode). */
    uint32_t        mode;
};

void        vfs_init(void);
vfs_node_t *vfs_lookup(const char *path);
int         vfs_open(const char *path, uint32_t flags);
int         vfs_close(int fd);
void        vfs_close_all_for_pid(uint32_t pid);
int64_t     vfs_read(int fd, void *buf, uint64_t len);
int64_t     vfs_write(int fd, const void *buf, uint64_t len);
int         vfs_mount(const char *mountpoint, vfs_node_t *root);
int64_t     vfs_readdir(int fd, void *buf, uint64_t max);
int         vfs_create(const char *path, uint8_t type);
int         vfs_unlink(const char *path);
int         vfs_chmod(const char *path, uint32_t mode);
int         vfs_chdir(const char *path);
int         vfs_getcwd(char *buf, uint64_t size);
vfs_node_t *vfs_get_cwd(void);
int64_t     vfs_lseek(int fd, int64_t offset, int whence);
int         vfs_dup(int fd);
int         vfs_dup2(int oldfd, int newfd);

/* stat structure */
typedef struct {
    uint64_t size;    /* file size in bytes */
    uint32_t type;    /* 0=file 1=dir 2=dev */
    uint64_t inode;
} vfs_stat_t;

int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_fstat(int fd, vfs_stat_t *st);