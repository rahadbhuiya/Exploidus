#pragma once

/*
 * Minimal ctype.h — plain ASCII classification, consistent with
 * locale.h always reporting the "C" locale (no locale-dependent
 * character tables needed).
 */

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int iscntrl(int c);
int ispunct(int c);
int isprint(int c);
int isgraph(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);