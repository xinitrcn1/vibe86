/* the firmware's freestanding <stdint.h> (C11 7.20), x86-64 LLP64/LP64 widths */
#ifndef FW_STDINT_H
#define FW_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef long long intptr_t;
typedef unsigned long long uintptr_t;
#define UINT64_MAX	0xFFFFFFFFFFFFFFFFull
#define UINT32_MAX	0xFFFFFFFFu
#endif
