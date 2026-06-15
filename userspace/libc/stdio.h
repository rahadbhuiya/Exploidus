#pragma once
#define _STDIO_H_
/*
 * Exploidus minimal stdio
 * Compatible with standard C stdio API subset
 */
#include "stdint.h"
#include "stddef.h"
#include "syscall.h"

/* FILE is a simple fd wrapper */
typedef struct {
    int fd;
    int error;
    int eof;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* printf family */
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, __builtin_va_list ap);
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);

/* puts/putchar */
int putchar(int c);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int fflush(FILE *f);

/* File operations */
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *buf, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f);
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
char *fgets(char *buf, int n, FILE *f);
int   getchar(void);

/* Convenience macros */
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define stdin  (stdin)
#define stdout (stdout)
#define stderr (stderr)