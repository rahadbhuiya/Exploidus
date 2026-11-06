#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SMP_MAX_CORES  32
#define SMP_STACK_SIZE 16384

typedef struct {
    uint32_t cpu_id;
    uint8_t  apic_id;
    bool     is_bsp;
    volatile bool online;
} smp_core_t;

extern smp_core_t g_smp_cores[SMP_MAX_CORES];
extern uint32_t   g_smp_online_count;

void smp_init(void);
uint32_t smp_get_core_count(void);
uint32_t smp_get_current_cpu(void);
bool smp_is_bsp(void);
