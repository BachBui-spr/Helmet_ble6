/*
 * Lop CORE - tron / am luong (xem mixer.h).
 */

#include <me54/core/mixer.h>
#include <me54/core/util.h>

void mixer_gain_q15(int16_t *s, uint32_t count, int32_t g_q15)
{
	for (uint32_t i = 0; i < count; i++) {
		s[i] = (int16_t)(((int32_t)s[i] * g_q15) >> 15);
	}
}

void mixer_add_mono(int16_t *stereo, const int16_t *mono, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		stereo[2 * i] = me54_sat16(stereo[2 * i] + mono[i]);
		stereo[2 * i + 1] = me54_sat16(stereo[2 * i + 1] + mono[i]);
	}
}
