#pragma once

/*
 * Minimal locale.h. Exploidus has no real locale data (no locale
 * files, no encoding tables beyond ASCII) — setlocale() always
 * reports/stays on the "C" locale regardless of what's requested.
 * This is a valid, well-precedented minimal implementation (same
 * approach many embedded/freestanding libcs take).
 */

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);