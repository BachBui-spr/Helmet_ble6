/*
 * Lop CORE: tien ich nho cho C thuan. File trong lib/core/ KHONG include <zephyr/...> de bien
 * dich va thu duoc tren PC (tests/core/).
 */

#ifndef ME54_CORE_UTIL_H_
#define ME54_CORE_UTIL_H_

#include <stdint.h>

#define ME54_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ME54_MIN(a, b)     (((a) < (b)) ? (a) : (b))
#define ME54_MAX(a, b)     (((a) > (b)) ? (a) : (b))
#define ME54_CLAMP(v, lo, hi) ME54_MIN(ME54_MAX((v), (lo)), (hi))

static inline int16_t me54_sat16(int32_t v)
{
	return (int16_t)ME54_CLAMP(v, INT16_MIN, INT16_MAX);
}

#endif /* ME54_CORE_UTIL_H_ */
