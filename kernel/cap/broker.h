#pragma once
#include "capability.h"
#include <stdbool.h>

/*
 * broker_check — validates that pid holds a token granting required_rights
 * on the given resource. Called at every syscall entry point.
 * Returns true if access is permitted, false if denied.
 */
bool broker_check(uint32_t pid, cap_token_t token, uint64_t required_rights);
