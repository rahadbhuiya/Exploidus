#pragma once
#include "process.h"

/* Timeslice lengths per intent in scheduler ticks */
#define SLICE_COMPUTE      20
#define SLICE_IO            5
#define SLICE_NETWORK       8
#define SLICE_INTERACTIVE   4
#define SLICE_AUDIT        10

void       sched_init(void);
void       sched_enqueue(process_t *proc);
void       sched_dequeue(process_t *proc);
void       sched_tick(void);          /* called from timer IRQ */
process_t *sched_next(void);
void       sched_yield(void);
extern uint64_t g_uptime_ticks;
