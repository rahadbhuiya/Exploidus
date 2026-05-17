#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Resource classes */
typedef enum {
    CAP_RES_FILE    = 0,
    CAP_RES_MEMORY  = 1,
    CAP_RES_PROCESS = 2,
    CAP_RES_DEVICE  = 3,
    CAP_RES_NETWORK = 4,
    CAP_RES_KERNEL  = 5,
    CAP_RES_COUNT   = 6
} cap_resource_t;

/* Rights bitmask (lower 16 bits of token.upper) */
#define CAP_RIGHT_READ         (1ULL << 0)
#define CAP_RIGHT_WRITE        (1ULL << 1)
#define CAP_RIGHT_EXEC         (1ULL << 2)
#define CAP_RIGHT_MAP          (1ULL << 3)
#define CAP_RIGHT_DELEGATE     (1ULL << 4)
#define CAP_RIGHT_REVOKE       (1ULL << 5)
#define CAP_RIGHT_NET_SEND     (1ULL << 6)
#define CAP_RIGHT_NET_RECV     (1ULL << 7)
#define CAP_RIGHT_NET_BIND     (1ULL << 8)
#define CAP_RIGHT_RAW_SOCKET   (1ULL << 9)
#define CAP_RIGHT_MOUNT        (1ULL << 10)
#define CAP_RIGHT_DRIVER_LOAD  (1ULL << 11)
#define CAP_RIGHT_SIGNAL       (1ULL << 12)
#define CAP_RIGHT_AUDIT        (1ULL << 13)

/*
 * cap_token_t — 128-bit capability token.
 *
 * upper layout:
 *   [63:48] resource class (cap_resource_t)
 *   [47:16] resource ID
 *   [15:0]  rights bitmask
 *
 * lower:
 *   BLAKE3(upper || owner_pid || kernel_secret)[0:7]
 */
typedef struct {
    uint64_t upper;
    uint64_t lower;
} cap_token_t;

#define CAP_NULL ((cap_token_t){0, 0})

/* Entry in a process's capability table */
typedef struct {
    cap_token_t token;
    uint32_t    owner_pid;
    uint64_t    expire_tick;   /* 0 = never */
    bool        revoked;
    bool        delegatable;
} cap_entry_t;

/* Subsystem init — must be called before any cap_create() */
void        cap_subsystem_init(void);

/* Mint a new token. Only callable from kernel context. */
cap_token_t cap_create(cap_resource_t res, uint32_t res_id,
                       uint64_t rights, uint32_t owner_pid);

/* Validate: returns true if token is authentic and grants required_rights */
bool        cap_validate(cap_token_t token, uint32_t pid,
                         uint64_t required_rights);

/* Revoke: invalidate `target` using `authority` as proof of revoke right.
 * authority must belong to revoker_pid and grant CAP_RIGHT_REVOKE for
 * the same resource class as target (CAP_RES_KERNEL authority revokes any). */
bool        cap_revoke(cap_token_t authority, cap_token_t target,
                       uint32_t revoker_pid);

/* Delegate: mint a subset-rights token for a different process */
cap_token_t cap_delegate(cap_token_t src, uint32_t src_pid,
                         uint32_t target_pid, uint64_t subset_rights);
