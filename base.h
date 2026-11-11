#ifndef BASE_H
#define BASE_H

#include <stdint.h>
#include <string.h>

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef s8  b8;
typedef s16 b16;
typedef s32 b32;
typedef s64 b64;

typedef float  f32;
typedef double f64;

#define false 0
#define true  1

#define KiB(n) ((n) << 10)
#define MiB(n) ((n) << 20)

#define assert(c) \
	do { \
		if (!(c)) { \
			*(int *)0 = 0; \
		} \
	} while (0)

typedef struct {
	u8 *data;
	u64 size;
} String8;

typedef struct {
	u32 *data;
	u64 size;
} String32;

typedef struct {
	u32 value;
	u32 size;
} Codepoint;

String8 str8(u8 *data, u64 size);
#define str8_lit(s) str8((u8 *)(s), sizeof(s)-1)

b32 str8_eq(String8 a, String8 b);

Codepoint decode_utf8(u8 *str, u64 max_size);

#endif
