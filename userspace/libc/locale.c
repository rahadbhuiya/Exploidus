#include "locale.h"

static struct lconv g_lconv = {
    ".", "", "", "", "", "", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127
};

struct lconv *localeconv(void)
{
    return &g_lconv;
}

char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return (char *)"C";
}