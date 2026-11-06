#pragma once
#include <stdint.h>
#include <stdbool.h>

void apic_init(uint64_t lapic_phys_addr);
void apic_eoi(void);
bool apic_is_enabled(void);
uint8_t apic_get_id(void);
void apic_send_ipi(uint8_t dest_apic_id, uint32_t dword_low);
void apic_init_ap(void);
