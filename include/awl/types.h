#ifndef AWL_TYPES_H
#define AWL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Dolphin SDK compatible type definitions */
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile u8  vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8  vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float  f32;
typedef double f64;
typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef int BOOL;

#define TRUE  1
#define FALSE 0

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

/* Utility macros */
#define ROUND_UP(x, align)   (((x) + (align) - 1) & ~((align) - 1))
#define ROUND_DOWN(x, align) ((x) & ~((align) - 1))
#define ROUND_UP_PTR(ptr, align) ((void*)ROUND_UP((uintptr_t)(ptr), (align)))
#define ROUND_DOWN_PTR(ptr, align) ((void*)ROUND_DOWN((uintptr_t)(ptr), (align)))

#define ALIGN(x) __attribute__((aligned(x)))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* AWL_TYPES_H */
