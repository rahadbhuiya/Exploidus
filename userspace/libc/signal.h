#pragma once

/*
 * Minimal signal.h. Honest limitation: Exploidus has no real
 * asynchronous signal-delivery mechanism yet (no way to interrupt a
 * running process from the keyboard IRQ or elsewhere) — see the
 * kill() honesty note in newlib_stubs.c for the related process-
 * targeting side of this.
 *
 * signal() here still does something useful: it records the handler
 * so a correct "previous handler" is returned on repeated calls (Lua
 * does exactly this — install a SIGINT handler, then later restore
 * SIG_DFL and expects the call to succeed) — it just never actually
 * *invokes* the handler, since nothing generates the interrupt. Once
 * a real signal-delivery path exists (keyboard IRQ -> deliver SIGINT
 * to the foreground process), this same table is where that would
 * hook in.
 */

typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGINT  2
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGSEGV 11
#define SIGTERM 15

sighandler_t signal(int signum, sighandler_t handler);