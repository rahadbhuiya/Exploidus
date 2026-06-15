#pragma once
/*
 * Exploidus minimal stdlib.h
 */
#include "stddef.h"
#include "stdint.h"

/* Memory */
void *malloc(size_t n);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t n);
void  free(void *ptr);

/* Process */
void  exit(int code) __attribute__((noreturn));
void  abort(void)    __attribute__((noreturn));

/* Conversion */
int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);

/* Sorting / searching */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*cmp)(const void *, const void *));

/* Random */
int  rand(void);
void srand(unsigned int seed);

/* Environment */
char *getenv(const char *name);

/* Absolute value */
int  abs(int n);
long labs(long n);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7fffffff
#define NULL         ((void *)0)