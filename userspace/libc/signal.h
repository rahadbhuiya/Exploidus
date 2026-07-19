#pragma once

/*
 * Minimal signal.h. Delivery currently only happens synchronously,
 * off a hardware fault in the calling process itself (divide error,
 * bad pointer, illegal instruction, see kernel/arch/x86_64/idt.c) --
 * there is still no asynchronous path (keyboard IRQ, another process
 * calling kill(), etc). See the kill() honesty note in
 * newlib_stubs.c for the process-targeting side of that.
 *
 * signal() records the handler so a correct "previous handler" is
 * returned on repeated calls (Lua does exactly this: install a
 * SIGINT handler, then later restore SIG_DFL and expects the call to
 * succeed), and also passes it to the kernel via sigaction_raw() so a
 * matching fault actually invokes it.
 *
 * A handler can end by calling exit(), or by calling sigreturn()
 * below to resume the code the fault interrupted instead of killing
 * the process.
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

/*
 * sigreturn -- call this at the end of a signal handler to resume the
 * code the fault interrupted, instead of calling exit(). Restores
 * every register (including the instruction pointer and stack) to
 * what it was at the moment of the fault. Never returns: control goes
 * straight back to the interrupted code, not to whatever called the
 * handler.
 */
void sigreturn(void) __attribute__((noreturn));