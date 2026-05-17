#pragma once
#include "idt.h"

typedef void (*irq_handler_fn)(interrupt_frame_t *frame);

void irq_init(void);
void irq_register(uint8_t irq, irq_handler_fn fn);
void irq_dispatch(interrupt_frame_t *frame);
void irq_eoi(uint8_t irq);
