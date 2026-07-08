#pragma once

/*
 * Minimal libm for Exploidus. Not IEEE-754-perfect, but accurate
 * enough for scripting-language interpreters (Lua-class languages) —
 * sin/cos/exp/log use range reduction + series/bit-trick approximations
 * good to roughly 1e-9 in normal ranges.
 */

#define M_PI 3.14159265358979323846
#define M_E  2.71828182845904523536

#define HUGE_VAL (__builtin_huge_val())

double fabs(double x);
float  fabsf(float x);

double sqrt(double x);
float  sqrtf(float x);

double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double trunc(double x);
double modf(double x, double *intpart);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sinh(double x);
double cosh(double x);
double tanh(double x);

double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double base, double exponent);

double frexp(double x, int *exp);
double ldexp(double x, int exp);