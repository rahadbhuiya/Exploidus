#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "vfs/vfs.h"

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t read_idx;
    uint32_t write_idx;
    uint32_t count;
    bool     read_open;
    bool     write_open;
} pipe_t;

int pipe_create(int fds[2]);
