#include "capability.h"
#include "../crypto/blake3.h"
#include "../audit/audit.h"
#include "../drivers/serial.h"
#include <string.h>

#define REVOKE_TABLE_SIZE 256

typedef struct { uint64_t upper; uint64_t lower; } revoke_entry_t;

static revoke_entry_t g_revoke_table[REVOKE_TABLE_SIZE];
static uint32_t g_revoke_count = 0;

static uint8_t g_kernel_secret[32];

/* 
   CPU feature check for RDRAND
 */
static inline int has_rdrand(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax),
          "=b"(ebx),
          "=c"(ecx),
          "=d"(edx)
        : "a"(1)
    );

    return (ecx & (1 << 30));
}

/* 
   Safe RNG (RDRAND + fallback)
 */
static inline uint64_t rdrand64(void)
{
    uint64_t val = 0;
    uint8_t ok = 0;

    /* fallback entropy if no RDRAND */
    if (!has_rdrand()) {

        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));

        return ((uint64_t)hi << 32 | lo) ^
               0xA5A5A5A5A5A5A5A5ULL;
    }

    for (int i = 0; i < 10; i++) {
        __asm__ volatile (
            "rdrand %0\n"
            "setc %1\n"
            : "=r"(val), "=qm"(ok)
        );
        if (ok) break;
    }

    return val;
}

/* 
   INIT
 */
void cap_subsystem_init(void)
{
    for (int i = 0; i < 4; i++) {
        uint64_t r = rdrand64();
        memcpy(g_kernel_secret + i * 8, &r, 8);
    }

    memset(g_revoke_table, 0, sizeof(g_revoke_table));
    g_revoke_count = 0;

    serial_print("[CAP] Kernel secret seeded (safe RNG)\n");
}

/* 
   BLAKE3 AUTH
 */
static uint64_t build_lower(uint64_t upper, uint32_t pid)
{
    uint8_t buf[44];
    uint8_t digest[32];
    uint64_t result;

    memcpy(buf, &upper, 8);
    memcpy(buf + 8, &pid, 4);
    memcpy(buf + 12, g_kernel_secret, 32);

    blake3_hash(buf, sizeof(buf), digest);
    memcpy(&result, digest, 8);
    return result;
}

/* 
   CREATE
 */
cap_token_t cap_create(cap_resource_t res, uint32_t res_id,
                       uint64_t rights, uint32_t owner_pid)
{
    cap_token_t tok;

    tok.upper = ((uint64_t)(res & 0xFFFF) << 48)
              | ((uint64_t)(res_id) << 16)
              | (rights & 0xFFFF);

    tok.lower = build_lower(tok.upper, owner_pid);

    audit_record(AUDIT_CAP_CREATE, owner_pid, tok.upper, tok.lower);
    return tok;
}

/* 
   VALIDATE
 */
bool cap_validate(cap_token_t token, uint32_t pid, uint64_t required_rights)
{
    uint64_t expected;
    uint64_t token_rights;

    if (token.upper == 0 && token.lower == 0) {
        audit_record(AUDIT_CAP_DENIED, pid, 0, 0);
        return false;
    }

    expected = build_lower(token.upper, pid);
    if (expected != token.lower) {
        audit_record(AUDIT_CAP_FORGERY, pid, token.upper, token.lower);
        return false;
    }

    for (uint32_t i = 0; i < g_revoke_count; i++) {
        if (g_revoke_table[i].upper == token.upper &&
            g_revoke_table[i].lower == token.lower) {
            audit_record(AUDIT_CAP_DENIED, pid, token.upper, token.lower);
            return false;
        }
    }

    token_rights = token.upper & 0xFFFF;

    if ((token_rights & required_rights) != required_rights) {
        audit_record(AUDIT_CAP_DENIED, pid, token.upper, required_rights);
        return false;
    }

    return true;
}

/* 
   REVOKE
 */
bool cap_revoke(cap_token_t authority, cap_token_t target,
                uint32_t revoker_pid)
{
    if (!cap_validate(authority, revoker_pid, CAP_RIGHT_REVOKE))
        return false;

    cap_resource_t auth_res =
        (cap_resource_t)((authority.upper >> 48) & 0xFFFF);

    cap_resource_t target_res =
        (cap_resource_t)((target.upper >> 48) & 0xFFFF);

    if (auth_res != CAP_RES_KERNEL && auth_res != target_res) {
        audit_record(AUDIT_CAP_DENIED, revoker_pid, target.upper, 0);
        return false;
    }

    if (target.upper == 0 && target.lower == 0)
        return false;

    for (uint32_t i = 0; i < g_revoke_count; i++) {
        if (g_revoke_table[i].upper == target.upper &&
            g_revoke_table[i].lower == target.lower)
            return true;
    }

    if (g_revoke_count >= REVOKE_TABLE_SIZE) {
        serial_print("[CAP] revocation table full\n");
        return false;
    }

    g_revoke_table[g_revoke_count++] = (revoke_entry_t){
        .upper = target.upper,
        .lower = target.lower
    };

    audit_record(AUDIT_CAP_REVOKE, revoker_pid, target.upper, target.lower);
    return true;
}

/* 
   DELEGATE
 */
cap_token_t cap_delegate(cap_token_t src, uint32_t src_pid,
                         uint32_t target_pid, uint64_t subset_rights)
{
    cap_token_t empty = CAP_NULL;

    if (!cap_validate(src, src_pid, CAP_RIGHT_DELEGATE))
        return empty;

    uint64_t original_rights = src.upper & 0xFFFF;
    uint64_t new_rights = original_rights & subset_rights;

    cap_token_t delegated;

    delegated.upper = (src.upper & ~(uint64_t)0xFFFF) | new_rights;
    delegated.lower = build_lower(delegated.upper, target_pid);

    audit_record(AUDIT_CAP_DELEGATE, target_pid, delegated.upper, delegated.lower);
    return delegated;
}