#pragma once

/*
 * Minimal assert.h. Real (not a no-op) implementation: prints the
 * failed expression/file/line to stderr and calls abort(), same
 * behavior as a normal libc's assert() when NDEBUG isn't defined.
 */

#ifdef NDEBUG

#define assert(cond) ((void)0)

#else

void __assert_fail(const char *expr, const char *file, int line)
    __attribute__((noreturn));

#define assert(cond) \
    ((cond) ? (void)0 : __assert_fail(#cond, __FILE__, __LINE__))

#endif