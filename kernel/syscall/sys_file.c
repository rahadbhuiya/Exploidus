#include "table.h"
#include "../fs/vfs/vfs.h"
#include "../fs/exfs/exfs.h"
#include "../fs/pipe.h"
#include "../drivers/blockdev.h"
#include "../drivers/serial.h"
#include "../proc/process.h"
#include "../audit/audit.h"
#include "../mm/kmalloc.h"
#include <string.h>

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

extern int64_t vfs_readdir(int fd, void *buf, uint64_t max);
extern int vfs_create(const char *path, uint8_t type);


int64_t sys_open(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path  = (const char *)(uintptr_t)f->rdi;
    uint32_t    flags = (uint32_t)f->rsi;

    uint64_t max_len = 4096;
    uint64_t i = 0;
    while (i < max_len && path[i]) i++;
    if (i == max_len) return -1;

    int fd = vfs_open(path, flags);

    if (fd < 0 && (flags & O_CREAT)) {
        int created_fd = vfs_create(path, 0);
        if (created_fd >= 0) vfs_close(created_fd);
        fd = vfs_open(path, flags);
    }

    if (fd < 0) {
        serial_print("[SYS] open: not found: ");
        serial_print(path);
        serial_print("\n");
    }

    if (g_current_proc)
        audit_record(AUDIT_FILE_OPEN, g_current_proc->pid, (uint64_t)(int64_t)fd, 0);

    return (int64_t)fd;
}



int64_t sys_close(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;
    if (fd < 3) return -1;
    return (int64_t)vfs_close(fd);
}


int64_t sys_lseek(syscall_frame_t *f)
{ return vfs_lseek((int)(int64_t)f->rdi,(int64_t)f->rsi,(int)(int64_t)f->rdx); }


int64_t sys_stat(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,sizeof(vfs_stat_t))) return -1;
    return (int64_t)vfs_stat((const char *)(uintptr_t)f->rdi,(vfs_stat_t *)(uintptr_t)f->rsi);
}


int64_t sys_fstat(syscall_frame_t *f)
{
    if (!uptr_ok(f->rsi,sizeof(vfs_stat_t))) return -1;
    return (int64_t)vfs_fstat((int)(int64_t)f->rdi,(vfs_stat_t *)(uintptr_t)f->rsi);
}


int64_t sys_dup(syscall_frame_t *f)
{ return (int64_t)vfs_dup((int)(int64_t)f->rdi); }


int64_t sys_dup2(syscall_frame_t *f)
{ return (int64_t)vfs_dup2((int)(int64_t)f->rdi,(int)(int64_t)f->rsi); }

int64_t sys_pipe(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(int) * 2)) return -1;
    int *user_fds = (int *)(uintptr_t)f->rdi;
    int kernel_fds[2];
    if (pipe_create(kernel_fds) < 0) return -1;
    user_fds[0] = kernel_fds[0];
    user_fds[1] = kernel_fds[1];
    return 0;
}




int64_t sys_readdir(syscall_frame_t *f)
{
    typedef struct { uint64_t inode; uint8_t type; char name[256]; } dirent_user_t;
    int fd = (int)(int64_t)f->rdi; uint64_t buf = f->rsi; uint64_t max = f->rdx;
    if (max > USER_ADDR_MAX / sizeof(dirent_user_t)) return -1;
    if (!uptr_ok(buf, max * sizeof(dirent_user_t))) return -1;
    return vfs_readdir(fd, (void *)(uintptr_t)buf, max);
}


int64_t sys_create(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    return (int64_t)vfs_create((const char *)(uintptr_t)f->rdi, (uint8_t)f->rsi);
}



int64_t sys_chdir(syscall_frame_t *f)
{ if (!uptr_ok(f->rdi,1)) return -1; return (int64_t)vfs_chdir((const char *)(uintptr_t)f->rdi); }


int64_t sys_getcwd(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,f->rsi)) return -1;
    return (int64_t)vfs_getcwd((char *)(uintptr_t)f->rdi, f->rsi);
}



/* sys_chmod(path, mode) */
int64_t sys_chmod(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    uint32_t    mode = (uint32_t)f->rsi;
    return vfs_chmod(path, mode);
}



/* sys_chgrp(path, gid) */
int64_t sys_chgrp(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    uint32_t    gid  = (uint32_t)f->rsi;
    return vfs_chgrp(path, gid);
}


int64_t sys_unlink(syscall_frame_t *f)
{ if (!uptr_ok(f->rdi,1)) return -1; return vfs_unlink((const char *)(uintptr_t)f->rdi); }



/* sys_rmdir(path) — refuses non-empty directories (see vfs_rmdir/exfs_op_rmdir) */
int64_t sys_rmdir(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    return vfs_rmdir(path);
}



/* sys_rename(old_path, new_path) */
int64_t sys_rename(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    if (!uptr_ok(f->rsi, 1)) return -1;
    const char *old_path = (const char *)(uintptr_t)f->rdi;
    const char *new_path = (const char *)(uintptr_t)f->rsi;
    return vfs_rename(old_path, new_path);
}



/* sys_symlink(target, linkpath) */
int64_t sys_symlink(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    if (!uptr_ok(f->rsi, 1)) return -1;
    const char *target = (const char *)(uintptr_t)f->rdi;
    const char *linkpath = (const char *)(uintptr_t)f->rsi;
    return vfs_symlink(target, linkpath);
}



/* sys_readlink(path, buf, bufsize) */
int64_t sys_readlink(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    char *buf = (char *)(uintptr_t)f->rsi;
    return vfs_readlink(path, buf, f->rdx);
}



int64_t sys_file_write(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,f->rdx)) return -1;
    const char *path=(const char *)(uintptr_t)f->rdi;
    const void *data=(const void *)(uintptr_t)f->rsi; uint64_t size=f->rdx;
    int fd=vfs_open(path,1);
    if(fd<0){int cfd=vfs_create(path,0);if(cfd<0)return -2;vfs_close(cfd);fd=vfs_open(path,1);if(fd<0)return -3;}
    int64_t written=vfs_write(fd,data,size); vfs_close(fd); return written;
}



/*
 * sys_mount(dev_name, mountpoint) — mount a registered block device
 * (e.g. "usb0", "ata0") as a fresh ExFS volume at a VFS path.
 * Deliberately minimal: LBA 0 only (no partition table support), no
 * unmount, no re-mount-over-existing-mountpoint check (vfs_mount()
 */
int64_t sys_mount(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1) || !uptr_ok(f->rsi, 1)) return -1;
    const char *dev_name    = (const char *)(uintptr_t)f->rdi;
    const char *mountpoint  = (const char *)(uintptr_t)f->rsi;

    block_device_t *dev = blockdev_find(dev_name);
    if (!dev) return -2;

    vfs_node_t *root = exfs_mount(dev, 0);
    if (!root) return -3;

    if (vfs_mount(mountpoint, root) != 0) return -4;

    return 0;
}



/*
 * sys_mkfs(dev_name, total_blocks) — format a registered block
 * device with a fresh ExFS volume at LBA 0 (see exfs_format()).
 * total_blocks is caller-specified rather than auto-derived from the
 * device's real capacity -- this driver doesn't have a generic
 */
int64_t sys_mkfs(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *dev_name = (const char *)(uintptr_t)f->rdi;
    uint64_t total_blocks = f->rsi;
    if (total_blocks == 0) return -1;

    block_device_t *dev = blockdev_find(dev_name);
    if (!dev) return -2;

    if (!exfs_format(dev, 0, total_blocks)) return -3;

    return 0;
}



/* sys_debug_exfs_crash(point) — TEST ONLY, see SYS_DEBUG_EXFS_CRASH */
int64_t sys_debug_exfs_crash(syscall_frame_t *f)
{
    int point = (int)(int64_t)f->rdi;
    if (point < 0 || point > 4) return -1;
    exfs_debug_arm_crash(point);
    return 0;
}



/* sys_debug_exfs_corrupt(path) — TEST ONLY, see SYS_DEBUG_EXFS_CORRUPT */
int64_t sys_debug_exfs_corrupt(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -1;
    int result = exfs_debug_corrupt_file(node);
    if (node->parent != NULL) { kfree(node->fs_data); kfree(node); }
    return result;
}


