/*
 * Lop CORE - vong PCM stereo + bu troi clock (xem pcm_ring.h).
 */

#include <me54/core/pcm_ring.h>

#define LEVEL_AVG_SHIFT 5   /* trung binh truot 1/32 */
#define LEVEL_SETTLE    100 /* khung truoc khi chot muc tieu (~1 s) */

void pcm_ring_init(struct pcm_ring *r, int16_t *buf, uint32_t cap)
{
	r->buf = buf;
	r->cap = cap;
	r->level_avg_x32 = 0;
	r->level_target = 0;
	pcm_ring_reset(r);
}

void pcm_ring_reset(struct pcm_ring *r)
{
	r->rd = 0;
	r->wr = 0;
	r->on = false;
	r->settle = 0;
}

static inline const int16_t *ring_at(const struct pcm_ring *r, uint32_t idx)
{
	return &r->buf[(idx % r->cap) * 2];
}

bool pcm_ring_push(struct pcm_ring *r, const int16_t *src, uint16_t n, uint32_t start_level)
{
	bool dropped = false;

	if (pcm_ring_count(r) + n > r->cap) {
		r->rd += n; /* day: bo khung cu nhat */
		dropped = true;
	}
	for (uint16_t i = 0; i < n; i++) {
		const uint32_t k = (r->wr + i) % r->cap;

		r->buf[2 * k] = src[2 * i];
		r->buf[2 * k + 1] = src[2 * i + 1];
	}
	r->wr += n;

	if (!r->on && pcm_ring_count(r) >= start_level) {
		r->on = true;
		r->settle = 0;
		r->level_target = (int32_t)pcm_ring_count(r);
		r->level_avg_x32 = r->level_target << LEVEL_AVG_SHIFT;
	}
	return dropped;
}

uint16_t pcm_ring_slip_take(struct pcm_ring *r, uint16_t n)
{
	const uint32_t level = pcm_ring_count(r);
	uint16_t take = n;
	int32_t avg;

	r->level_avg_x32 += (int32_t)level - (r->level_avg_x32 >> LEVEL_AVG_SHIFT);
	avg = r->level_avg_x32 >> LEVEL_AVG_SHIFT;

	if (r->settle < LEVEL_SETTLE) {
		if (++r->settle == LEVEL_SETTLE) {
			r->level_target = avg; /* chot muc tieu sau ~1 s on dinh */
		}
	} else {
		const int32_t hyst = n / 5;

		if (avg > r->level_target + hyst && level >= (uint32_t)n + 1U) {
			take = n + 1;
		} else if (avg < r->level_target - hyst) {
			take = n - 1;
		}
	}
	return take;
}

void pcm_ring_pull(struct pcm_ring *r, int16_t *dst, uint16_t n, uint16_t take)
{
	if (take == n) {
		for (uint16_t j = 0; j < n; j++) {
			const int16_t *s = ring_at(r, r->rd + j);

			dst[2 * j] = s[0];
			dst[2 * j + 1] = s[1];
		}
	} else {
		const uint32_t step = ((uint32_t)(take - 1) << 16) / (n - 1);
		uint32_t pos = 0;

		for (uint16_t j = 0; j < n; j++) {
			const uint32_t i = pos >> 16;
			const int32_t f = (int32_t)(pos & 0xFFFF);
			const int16_t *a = ring_at(r, r->rd + i);
			const int16_t *b = (i + 1 < take) ? ring_at(r, r->rd + i + 1) : a;

			dst[2 * j] = (int16_t)(a[0] + (((b[0] - a[0]) * f) >> 16));
			dst[2 * j + 1] = (int16_t)(a[1] + (((b[1] - a[1]) * f) >> 16));
			pos += step;
		}
	}
	r->rd += take;
}
