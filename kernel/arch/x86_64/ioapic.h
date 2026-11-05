#pragma once
#include <stdint.h>
#include <stdbool.h>

void ioapic_init(uint64_t ioapic_phys_addr);
bool ioapic_is_enabled(void);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, bool masked);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);
