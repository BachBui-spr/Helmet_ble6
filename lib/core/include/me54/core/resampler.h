/*
 * Lop CORE: lay mau lai ti le CO DINH num / den (mau vao / mau ra), noi suy Hermite
 * (Catmull-Rom), stereo int16 xen ke. Bo dem pha la so nguyen nen khong tich luy sai so.
 *
 * Mau vao lay theo tung khung qua callback refill (vd services/audio_out.c dung khung danh dinh
 * 10 ms). num == den: chep thang, khong noi suy.
 */

#ifndef ME54_CORE_RESAMPLER_H_
#define ME54_CORE_RESAMPLER_H_

#include <stdint.h>

/* Cap mot khung mau vao moi: *buf = mau stereo xen ke, *len = so mau stereo (> 0) */
typedef void (*resampler_refill_fn)(void *ctx, const int16_t **buf, uint16_t *len);

struct resampler {
	uint32_t num;
	uint32_t den;
	uint32_t frac;
	float h[4][2]; /* x[-1], x[0], x[1], x[2] cua tung kenh */
	const int16_t *in;
	uint16_t in_len;
	uint16_t in_idx;
	resampler_refill_fn refill;
	void *ctx;
};

void resampler_init(struct resampler *r, resampler_refill_fn refill, void *ctx);
/* Doi ti le (giu pha / lich su); goi resampler_reset() khi bat dau phat lai */
void resampler_set_ratio(struct resampler *r, uint32_t num, uint32_t den);
/* Bo khung vao dang do (lan lay ke tiep goi refill) */
void resampler_drop_input(struct resampler *r);
/* Bo khung vao + pha + lich su noi suy */
void resampler_reset(struct resampler *r);
/* Ghi n mau stereo ra dst */
void resampler_process(struct resampler *r, int16_t *dst, uint16_t n);

/* 16 -> 32 kHz mono: mau chan = mau goc, mau le = Hermite tai diem giua (tre 1 mau).
 * h: lich su 4 mau (dat 0 khi bat dau), out: 2 * n mau.
 */
void resampler_up2(float h[4], const int16_t *in, int16_t *out, uint16_t n);

#endif /* ME54_CORE_RESAMPLER_H_ */
