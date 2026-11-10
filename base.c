#include "base.h"

static u8 utf8_class[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 4, 5
};

String8 str8(u8 *data, u64 size)
{
	String8 s = {.data=data, .size=size};
	return s;
}

b32 str8_eq(String8 a, String8 b)
{
	if (a.size != b.size) {
		return 0;
	}
	return a.size == 0 || memcmp(a.data, b.data, a.size) == 0;
}

Codepoint decode_utf8(u8 *str, u64 max_size)
{
	Codepoint point = {
		.value = ~(u32)0,
		.size = 1
	};

	u8 size = utf8_class[str[0] >> 3];

	switch (size) {
	case 1:
		point.value = str[0];
		point.size = size;
		break;
	case 2:
		if (size <= max_size && utf8_class[str[1] >> 3] == 0) {
			point.value = (str[0] & 0x1f) << 6;
			point.value |= str[1] & 0x3f;
			point.size = size;
		}
		break;
	case 3:
		if (size <= max_size && utf8_class[str[1] >> 3] == 0 && utf8_class[str[2] >> 3] == 0) {
			point.value = (str[0] & 0xf) << 12;
			point.value |= (str[1] & 0x3f) << 6;
			point.value |= str[2] & 0x3f;
			point.size = size;
		}
		break;
	case 4:
		if (size <= max_size && utf8_class[str[1] >> 3] == 0 && utf8_class[str[2] >> 3] == 0 && utf8_class[str[3] >> 3] == 0) {
			point.value = (str[0] & 0x7) << 18;
			point.value |= (str[1] & 0x3f) << 12;
			point.value |= (str[2] & 0x3f) << 6;
			point.value |= str[3] & 0x3f;
			point.size = size;
		}
		break;
	}

	return point;
}
