#pragma once

/*
 * Minimal errno.h. Newlib (and most ported POSIX code) reads/writes
 * a global `errno` extensively, so this needs to exist before any
 * serious porting work. Values match the standard POSIX/Linux
 * numbering so code with hardcoded expectations (rare, but it
 * happens) still works.
 */

extern int errno;

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define EBADF        9
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOSPC      28
#define ESPIPE      29
#define EROFS       30
#define ENOSYS      38
#define ENOTEMPTY   39