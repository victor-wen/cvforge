/*
 * Minimal freestanding <stdint.h> stub used ONLY by the CVF-001 header-isolation
 * compile check (tests/abi/test_header_isolation.c). The public header is
 * compiled with -nostdinc against this directory, so a dependency on any other
 * standard, C++ library, OpenCV, vendor, or Windows header becomes a compile
 * error. Do not use this stub anywhere else.
 */
#ifndef CVF_TEST_ISOLATION_STDINT_H
#define CVF_TEST_ISOLATION_STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef int8_t   int_least8_t;
typedef uint8_t  uint_least8_t;
typedef int16_t  int_least16_t;
typedef uint16_t uint_least16_t;
typedef int32_t  int_least32_t;
typedef uint32_t uint_least32_t;
typedef int64_t  int_least64_t;
typedef uint64_t uint_least64_t;

typedef signed char        int_fast8_t;
typedef unsigned char      uint_fast8_t;
typedef signed short       int_fast16_t;
typedef unsigned short     uint_fast16_t;
typedef signed int         int_fast32_t;
typedef unsigned int       uint_fast32_t;
typedef signed long long   int_fast64_t;
typedef unsigned long long uint_fast64_t;

typedef signed long        intptr_t;
typedef unsigned long      uintptr_t;
typedef signed long long   intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255u
#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define UINT16_MAX 65535u
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295u
#define INT64_MIN (-9223372036854775807LL - 1)
#define INT64_MAX 9223372036854775807LL
#define UINT64_MAX 18446744073709551615ULL
#define INTPTR_MIN (-9223372036854775807L - 1)
#define INTPTR_MAX 9223372036854775807L
#define UINTPTR_MAX 18446744073709551615UL
#define INTMAX_MIN (-9223372036854775807LL - 1)
#define INTMAX_MAX 9223372036854775807LL
#define UINTMAX_MAX 18446744073709551615ULL
#define SIZE_MAX UINTPTR_MAX
#define PTRDIFF_MIN INTPTR_MIN
#define PTRDIFF_MAX INTPTR_MAX

#endif /* CVF_TEST_ISOLATION_STDINT_H */
