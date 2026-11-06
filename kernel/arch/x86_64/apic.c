#include "apic.h"
#include <stdint.h>
#include <stdbool.h>

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

bool apic_is_enabled(void)
{
    return apic != 0;
}

void apic_init(uint64_t lapic_phys_addr)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);

    base |= APIC_ENABLE;
    if (lapic_phys_addr) {
        base = (base & ~APIC_MASK) | (lapic_phys_addr & APIC_MASK);
    }

    wrmsr(MSR_APIC_BASE, base);

    apic = (volatile uint32_t *)(base & APIC_MASK);

    if (!apic)
        return;

    /* enable APIC */
    uint32_t svr = apic_read(APIC_SVR);
    svr |= (1 << 8);   // enable APIC software
    svr |= 0xFF;       // spurious vector 0xFF
    apic_write(APIC_SVR, svr);
}

void apic_eoi(void)
{
    if (apic)
        apic_write(APIC_EOI, 0);
}

uint8_t apic_get_id(void)
{
    if (!apic) return 0;
    return (uint8_t)((apic_read(0x20) >> 24) & 0xFF);
}

void apic_send_ipi(uint8_t dest_apic_id, uint32_t dword_low)
{
    if (!apic) return;
    /* Wait for delivery status to clear (bit 12) */
    while (apic_read(0x300) & (1 << 12)) {
        __asm__ volatile ("pause");
    }
    /* Write ICR high (bits 24-31 = destination APIC ID) */
    apic_write(0x310, (uint32_t)dest_apic_id << 24);
    /* Write ICR low to dispatch IPI */
    apic_write(0x300, dword_low);
}

void apic_init_ap(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    base |= APIC_ENABLE;
    wrmsr(MSR_APIC_BASE, base);

    if (!apic) {
        apic = (volatile uint32_t *)(base & APIC_MASK);
    }

    /* Software enable APIC and set spurious vector to 0xFF */
    uint32_t svr = apic_read(APIC_SVR);
    svr |= (1 << 8);
    svr |= 0xFF;
    apic_write(APIC_SVR, svr);

    /* Clear Task Priority Register (TPR) */
    apic_write(0x80, 0);
}
