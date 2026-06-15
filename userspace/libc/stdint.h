#pragma once
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned long long uintptr_t;
typedef long long          intptr_t;

#ifndef _SYSCALL_H_TYPES_DEFINED
#define _SYSCALL_H_TYPES_DEFINED
typedef unsigned long long size_t;
typedef long long          ssize_t;
#endif

#define UINT8_MAX   0xff
#define UINT16_MAX  0xffff
#define UINT32_MAX  0xffffffffU
#define UINT64_MAX  0xffffffffffffffffULL
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define INT32_MIN   (-2147483648)
#define INT32_MAX   2147483647
#define INT64_MIN   (-9223372036854775808LL)
#define INT64_MAX   9223372036854775807LL
#define SIZE_MAX    UINT64_MAX