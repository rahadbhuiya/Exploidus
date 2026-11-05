#pragma once
#include <stdint.h>
#include <stdbool.h>

void apic_init(uint64_t lapic_phys_addr);
void apic_eoi(void);
bool apic_is_enabled(void);
