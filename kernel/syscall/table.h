#pragma once
#include <stdint.h>
#include "../cap/capability.h"

/* Exploidus syscall numbers */
#define SYS_EXIT          0
#define SYS_FORK          1
#define SYS_EXEC          2
#define SYS_WAITPID       3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_READ          6
#define SYS_WRITE         7
#define SYS_MMAP          8
#define SYS_MUNMAP        9
#define SYS_CAP_CREATE   10
#define SYS_CAP_DELEGATE 11
#define SYS_CAP_REVOKE   12
#define SYS_SET_INTENT   13
#define SYS_AUDIT_READ   14
#define SYS_YIELD        15
#define SYS_GETPID       16
#define SYS_SLEEP        17
#define SYS_SOCKET       18
#define SYS_BIND         19
#define SYS_CONNECT      20
#define SYS_LISTEN       21
#define SYS_ACCEPT       22
#define SYS_NET_SEND     23
#define SYS_NET_RECV     24
#define SYS_NET_CLOSE    25
#define SYS_GETIFADDR    26
#define SYS_AUDIT_DUMP   27
#define SYS_GETPROCS     28

#define SYS_FB_PIXEL     29
#define SYS_FB_RECT      30
#define SYS_FB_CLEAR     31
#define SYS_POWEROFF     32
#define SYS_REBOOT       33
#define SYS_FB_INFO      34
#define SYS_READDIR      35
#define SYS_CREATE       36
#define SYS_MOUSE_POS    37
#define SYS_FB_STR       38

#define SYS_SPAWN        39
#define SYS_UPTIME       40
#define SYS_PING         41
#define SYS_FB_CIRCLE    42
#define SYS_FB_RRECT     43
#define SYS_FB_FLIP      44
#define SYS_CHDIR        45
#define SYS_GETCWD         46
#define SYS_CNSL_UNBLOCK   47
#define SYS_CNSL_BLOCK_TTL 48
#define SYS_CNSL_LIST      49
#define SYS_LSEEK          50
#define SYS_STAT           51
#define SYS_FSTAT          52
#define SYS_DUP            53
#define SYS_DUP2           54
#define SYS_HTTP_GET       55   /* download URL to buffer */
#define SYS_FILE_WRITE     56   /* create+write file atomically */
#define SYS_COUNT          57




/*
 * syscall_frame_t
 *
 * Field order MUST exactly match the push sequence in entry.asm.
 * entry.asm pushes (last push = lowest address = offset 0):
 *
 *   push user_rsp  -> offset 72
 *   push rcx       -> offset 64  (user RIP)
 *   push r11       -> offset 56  (user RFLAGS)
 *   push rax       -> offset 48  (syscall nr in; return value out)
 *   push rdi       -> offset 40  (arg0)
 *   push rsi       -> offset 32  (arg1)
 *   push rdx       -> offset 24  (arg2)
 *   push r10       -> offset 16  (arg3)
 *   push r8        -> offset  8  (arg4)
 *   push r9        -> offset  0  (arg5)  <-- rsp points here
 */
typedef struct __attribute__((packed)) {
    uint64_t r9;       /* offset  0 */
    uint64_t r8;       /* offset  8 */
    uint64_t r10;      /* offset 16 */
    uint64_t rdx;      /* offset 24 */
    uint64_t rsi;      /* offset 32 */
    uint64_t rdi;      /* offset 40 */
    uint64_t rax;      /* offset 48 — syscall number on entry, return value on exit */
    uint64_t r11;      /* offset 56 — user RFLAGS */
    uint64_t rcx;      /* offset 64 — user RIP    */
    uint64_t user_rsp; /* offset 72 — user RSP    */
} syscall_frame_t;

void syscall_init(void);
void syscall_dispatch(syscall_frame_t *frame);