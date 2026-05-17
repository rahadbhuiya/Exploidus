#include "apic.h"
#include <stdint.h>

#define MSR_APIC_BASE     0x1B
#define APIC_ENABLE       (1ULL << 11)
#define APIC_MASK         0xFFFFF000ULL

#define APIC_SVR          0xF0
#define APIC_EOI          0xB0

static volatile uint32_t *apic = 0;

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
        : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline void apic_write(uint32_t reg, uint32_t val)
{
    apic[reg / 4] = val;
    __asm__ volatile ("" ::: "memory");
}

static inline uint32_t apic_read(uint32_t reg)
{
    return apic[reg / 4];
}

void apic_init(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);

    base |= APIC_ENABLE;

    wrmsr(MSR_APIC_BASE, base);

    apic = (volatile uint32_t *)(base & APIC_MASK);

    if (!apic)
        return;

    /* enable APIC */
    uint32_t svr = apic_read(APIC_SVR);
    svr |= (1 << 8);   // enable
    svr |= 0xFF;       // spurious vector
    apic_write(APIC_SVR, svr);
}

void apic_eoi(void)
{
    if (apic)
        apic_write(APIC_EOI, 0);
}