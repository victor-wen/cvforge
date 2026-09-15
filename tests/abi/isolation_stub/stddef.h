/*
 * Minimal freestanding <stddef.h> stub used ONLY by the CVF-001 header-isolation
 * compile check (tests/abi/test_header_isolation.c). See isolation_stub/stdint.h.
 */
#ifndef CVF_TEST_ISOLATION_STDDEF_H
#define CVF_TEST_ISOLATION_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#define NULL ((void*)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* CVF_TEST_ISOLATION_STDDEF_H */
