#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VFS_NAME_MAX  255
#define VFS_MAX_FDS   64

typedef enum {
    VFS_FILE      = 0,
    VFS_DIRECTORY = 1,
    VFS_DEVICE    = 2,
    VFS_SYMLINK   = 3,
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
    int      (*rmdir) (vfs_node_t *dir, const char *name); /* refuses non-empty dirs (-2) */
    int      (*chmod) (vfs_node_t *node, uint32_t mode); /* persist mode to disk; NULL = unsupported */
    /* Move/rename a dirent from (old_dir,old_name) to (new_dir,new_name).
     * old_dir and new_dir are always on the same mounted filesystem
     * (vfs_rename() refuses cross-filesystem moves before calling this,
     * same EXDEV-style restriction real rename(2) has). NULL = unsupported. */
    int      (*rename)(vfs_node_t *old_dir, const char *old_name,
                        vfs_node_t *new_dir, const char *new_name);
    /* Create a symlink dirent named `name` in `dir`, whose stored
     * content is the raw target path string `target` (not
     * interpreted or validated at creation time -- a dangling or
     * relative target is not an error here, same as real symlink(2)).
     * NULL = unsupported. */
    int      (*symlink)(vfs_node_t *dir, const char *name, const char *target);
    /* Persist a new group_gid to disk; NULL = unsupported. Same
     * authorization model as chmod above -- no owner/root check here
     * either, matching chmod's existing (pre-existing, not something
     * this added) lack of one. */
    int      (*chgrp)(vfs_node_t *node, uint32_t gid);
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

    /* Owning user ID (see process_t.uid / exfs_inode_t.owner_uid).
     * Default UID_ROOT for filesystems/pseudo-nodes that don't track
     * an owner -- combined with vfs_open()'s "UID_ROOT bypasses all
     * checks" rule, that's the same "fully permissive by default"
     * behavior as the mode field above, for the same reason. */
    uint32_t        owner_uid;

    /* Owning group ID (see process_t.gid / exfs_inode_t.group_gid).
     * Default GID_ROOT, same reasoning as owner_uid above. */
    uint32_t        group_gid;
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
int         vfs_rmdir(const char *path);
int         vfs_rename(const char *old_path, const char *new_path);
/* vfs_lookup() follows symlinks transparently, including the final
 * path component (so opening/reading/writing through a symlink just
 * works) -- bounded by an internal depth limit against cycles.
 * vfs_lookup_link() resolves intermediate components the same way
 * but returns the symlink node itself if the FINAL component is a
 * symlink, instead of following it -- for readlink()/lstat-style
 * callers that want the link, not its target. */
vfs_node_t *vfs_lookup_link(const char *path);
int         vfs_symlink(const char *target, const char *linkpath);
int64_t     vfs_readlink(const char *path, char *buf, uint64_t bufsize);
int         vfs_chgrp(const char *path, uint32_t gid);
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