#pragma once
/* All types come from stdint.h / syscall.h to avoid conflicts */
#include "stdint.h"
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
typedef long ptrdiff_t;