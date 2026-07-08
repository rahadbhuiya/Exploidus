#include "signal.h"
#include <stddef.h>

#define MAX_SIGNALS 32

static sighandler_t g_handlers[MAX_SIGNALS];

sighandler_t signal(int signum, sighandler_t handler)
{
    if (signum < 0 || signum >= MAX_SIGNALS) return SIG_ERR;
    sighandler_t prev = g_handlers[signum];
    if (prev == NULL) prev = SIG_DFL; /* default until set otherwise */
    g_handlers[signum] = handler;
    return prev;
}