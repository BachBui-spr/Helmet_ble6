/*
 * Lop CORE: tron / am luong tren khung stereo int16 xen ke.
 */

#ifndef ME54_CORE_MIXER_H_
#define ME54_CORE_MIXER_H_

#include <stdint.h>

/* count mau (ca hai kenh): s[i] = s[i] * g / 32768 (g Q15, 32767 = 0 dB) */
void mixer_gain_q15(int16_t *s, uint32_t count, int32_t g_q15);
/* Cong n mau mono vao ca hai kenh cua khung stereo (bao hoa) */
void mixer_add_mono(int16_t *stereo, const int16_t *mono, uint16_t n);

#endif /* ME54_CORE_MIXER_H_ */
