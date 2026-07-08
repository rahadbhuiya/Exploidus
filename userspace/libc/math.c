#include "math.h"


/* Native-instruction ones — the compiler lowers these to a single */
/* SSE instruction (no external libm call), so they're free/exact. */


double fabs(double x)  { return __builtin_fabs(x); }
float  fabsf(float x)  { return __builtin_fabsf(x); }
double sqrt(double x)  { return __builtin_sqrt(x); }
float  sqrtf(float x)  { return __builtin_sqrtf(x); }


/* floor / ceil / trunc / fmod */


double trunc(double x)
{
    return (double)(long long)x;
}

double floor(double x)
{
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}

double fmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    double t = trunc(x / y);
    return x - t * y;
}


/* sin / cos / tan — range-reduce to [-pi, pi], then Taylor series */


#define TWO_PI (2.0 * M_PI)

static double reduce_angle(double x)
{
    double n = x / TWO_PI;
    long long k = (long long)(n + (n >= 0.0 ? 0.5 : -0.5));
    return x - (double)k * TWO_PI;
}

double sin(double x)
{
    x = reduce_angle(x);
    double x2 = x * x;
    double term = x, sum = x;
    for (int n = 1; n <= 12; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum  += term;
    }
    return sum;
}

double cos(double x)
{
    x = reduce_angle(x);
    double x2 = x * x;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n <= 12; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum  += term;
    }
    return sum;
}

double tan(double x)
{
    double c = cos(x);
    if (c == 0.0) c = 1e-300; /* avoid div-by-zero near pi/2 */
    return sin(x) / c;
}

/*
 * atan via the half-angle identity atan(x) = 2*atan(x/(1+sqrt(1+x^2))),
 * applied repeatedly to shrink x into the range where a Taylor series
 * converges quickly and accurately, then doubling back out.
 */
double atan(double x)
{
    int sign = 1;
    if (x < 0.0) { sign = -1; x = -x; }

    int inverted = (x > 1.0);
    if (inverted) x = 1.0 / x;

    int halvings = 0;
    while (x > 0.1 && halvings < 8) {
        x = x / (1.0 + sqrt(1.0 + x * x));
        halvings++;
    }

    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n <= 12; n++) {
        term *= -x2;
        sum  += term / (double)(2 * n + 1);
    }

    for (int i = 0; i < halvings; i++) sum *= 2.0;
    if (inverted) sum = M_PI / 2.0 - sum;
    return sign * sum;
}

double asin(double x)
{
    if (x >= 1.0)  return M_PI / 2.0;
    if (x <= -1.0) return -M_PI / 2.0;
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x)
{
    return M_PI / 2.0 - asin(x);
}

double atan2(double y, double x)
{
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return atan(y / x) + (y >= 0.0 ? M_PI : -M_PI);
    if (y > 0.0) return M_PI / 2.0;
    if (y < 0.0) return -M_PI / 2.0;
    return 0.0; /* atan2(0,0): conventionally 0 */
}

double sinh(double x) { return (exp(x) - exp(-x)) / 2.0; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2.0; }
double tanh(double x)
{
    if (x > 20.0) return 1.0;   /* avoid needless overflow, converges to 1 */
    if (x < -20.0) return -1.0;
    double e2x = exp(2.0 * x);
    return (e2x - 1.0) / (e2x + 1.0);
}


/* exp / log / pow */


#define LN2 0.6931471805599453

double exp(double x)
{
    if (x == 0.0) return 1.0;

    /* x = k*ln2 + r, exp(x) = 2^k * exp(r), r kept small for the series */
    long long k = (long long)(x / LN2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)k * LN2;

    double term = 1.0, sum = 1.0;
    for (int n = 1; n <= 15; n++) {
        term *= r / (double)n;
        sum  += term;
    }

    /* multiply by 2^k directly via the exponent bits (fast ldexp) */
    union { double d; unsigned long long u; } v;
    v.d = sum;
    v.u = v.u + (((unsigned long long)k) << 52);
    return v.d;
}

double log(double x)
{
    if (x <= 0.0) return -1.0 / 0.0; /* -inf for x<=0, matches libm convention */

    /* x = m * 2^e with 1 <= m < 2 (bit-trick frexp) */
    union { double d; unsigned long long u; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;
    v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = v.d;

    /* ln(m) = 2*atanh((m-1)/(m+1)) — converges fast for m in [1,2) */
    double u = (m - 1.0) / (m + 1.0);
    double u2 = u * u;
    double term = u, sum = u;
    for (int n = 1; n <= 15; n++) {
        term *= u2;
        sum  += term / (double)(2 * n + 1);
    }

    return 2.0 * sum + (double)e * LN2;
}

double pow(double base, double exponent)
{
    if (base == 0.0) return (exponent == 0.0) ? 1.0 : 0.0;

    /* fast path for small non-negative integer exponents */
    if (exponent == trunc(exponent) && exponent >= 0.0 && exponent < 64.0) {
        double result = 1.0, b = base;
        long long e = (long long)exponent;
        while (e > 0) {
            if (e & 1) result *= b;
            b *= b;
            e >>= 1;
        }
        return result;
    }

    return exp(exponent * log(base));
}

double log2(double x)  { return log(x) / LN2; }
double log10(double x) { return log(x) / 2.302585092994046; } /* ln(10) */

double modf(double x, double *intpart)
{
    double ip = trunc(x);
    *intpart = ip;
    return x - ip;
}

/* frexp: x = m * 2^e, m in [0.5, 1). Bit-trick, same family as the
 * exponent extraction in log() above. */
double frexp(double x, int *exp)
{
    if (x == 0.0) { *exp = 0; return 0.0; }
    union { double d; unsigned long long u; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF) - 1022;
    v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    *exp = e;
    return v.d;
}

/* ldexp: x * 2^exp, direct exponent-field manipulation (same trick
 * used for the 2^k multiply inside exp() above). */
double ldexp(double x, int exp)
{
    if (x == 0.0) return x;
    union { double d; unsigned long long u; } v;
    v.d = x;
    v.u = v.u + (((unsigned long long)(long long)exp) << 52);
    return v.d;
}