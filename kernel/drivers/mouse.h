#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t x, y;
    bool    left, right, middle;
} mouse_state_t;

extern mouse_state_t g_mouse;

void mouse_init(void);
void mouse_draw_cursor(void);
void mouse_erase_cursor(void);
