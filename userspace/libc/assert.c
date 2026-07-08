#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", expr, file, line);
    abort();
    __builtin_unreachable();
}