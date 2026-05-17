#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../proc/process.h"

/*
 * fork — create an exact copy of the calling process.
 * Returns the child PID in the parent, 0 in the child.
 * Returns -1 on failure.
 */
int64_t sys_fork_impl(void);

/*
 * exec — replace the current process image with a new ELF binary.
 * elf_data : pointer to ELF bytes in kernel memory
 * elf_size : size in bytes
 * Returns 0 on success (never actually returns — jumps to entry).
 * Returns -1 on failure (process image is unchanged).
 */
int64_t sys_exec_impl(const uint8_t *elf_data, uint64_t elf_size);

/*
 * waitpid — block until child process with given pid exits.
 * Returns exit code, or -1 if no such child.
 */
int64_t sys_waitpid_impl(uint32_t pid);

/*
 * jump_to_userspace — switch CPU to ring 3 and begin executing at entry.
 * This function does not return.
 */
void jump_to_userspace(uint64_t entry, uint64_t stack_top,
                       uint64_t pml4_phys) __attribute__((noreturn));
