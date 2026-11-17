/* the firmware's freestanding <stddef.h> (C11 7.19) */
#ifndef FW_STDDEF_H
#define FW_STDDEF_H
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define NULL		((void *)0)
#define offsetof(t, m)	__builtin_offsetof(t, m)
#endif
