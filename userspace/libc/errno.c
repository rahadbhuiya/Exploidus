#include "errno.h"

/*
 * Single global errno — Exploidus is one-thread-per-process today, so
 * a plain global is correct. If/when real shared-address-space
 * threads exist (see kernel/sync/futex.h notes), this needs to become
 * thread-local (backed by the fs_base/TLS mechanism already added)
 * instead, same as glibc/newlib's reentrant errno.
 */
int errno = 0;