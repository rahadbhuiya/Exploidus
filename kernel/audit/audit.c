#include "audit.h"
#include <string.h>

static audit_entry_t g_ring[AUDIT_RING_SIZE];
static uint32_t      g_head   = 0;   /* next write position */
static uint64_t      g_total  = 0;   /* total events ever recorded */
static uint64_t      g_tick   = 0;   /* monotonic tick counter */

void audit_init(void)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_head  = 0;
    g_total = 0;
    g_tick  = 0;
}

void audit_record(audit_event_t ev, uint32_t pid,
                  uint64_t arg0, uint64_t arg1)
{
    uint32_t idx = g_head & (AUDIT_RING_SIZE - 1);

    g_ring[idx].timestamp = g_tick++;
    g_ring[idx].pid       = pid;
    g_ring[idx].event     = ev;
    g_ring[idx].arg0      = arg0;
    g_ring[idx].arg1      = arg1;

    g_head++;
    g_total++;
}

const audit_entry_t *audit_read(uint32_t *count_out)
{
    uint32_t filled = (g_total < AUDIT_RING_SIZE)
                    ? (uint32_t)g_total
                    : AUDIT_RING_SIZE;
    if (count_out) *count_out = filled;
    return g_ring;
}

uint64_t audit_total(void)
{
    return g_total;
}
