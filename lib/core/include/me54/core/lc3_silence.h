/*
 * Lop CORE: ma hoa san mot khung LC3 "im lang" (PCM toan 0). Ben nhan giai ma ra im lang sach,
 * khac voi bo trong goi (ben nhan phai noi suy PLC).
 *
 * lc3_encode can ~2,5 KB stack: chi goi tu thread co stack du (vd main, workqueue 4 KB).
 */

#ifndef ME54_CORE_LC3_SILENCE_H_
#define ME54_CORE_LC3_SILENCE_H_

#include <stdint.h>

/* freq_hz <= 48000, frame_us <= 10000. out: octets byte. Tra ve 0 hoac so am */
int lc3_silence_frame(uint32_t frame_us, uint32_t freq_hz, uint16_t octets, uint8_t *out);

#endif /* ME54_CORE_LC3_SILENCE_H_ */
