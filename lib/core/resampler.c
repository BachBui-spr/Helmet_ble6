/*
 * Lop CORE - lay mau lai ti le co dinh (xem resampler.h).
 */

#include <string.h>

#include <me54/core/resampler.h>
#include <me54/core/util.h>

void resampler_init(struct resampler *r, resampler_refill_fn refill, void *ctx)
{
	memset(r, 0, sizeof(*r));
	r->num = 1;
	r->den = 1;
	r->refill = refill;
	r->ctx = ctx;
}

void resampler_set_ratio(struct resampler *r, uint32_t num, uint32_t den)
{
	r->num = num;
	r->den = den;
}

void resampler_drop_input(struct resampler *r)
{
	r->in_len = 0;
	r->in_idx = 0;
}

void resampler_reset(struct resampler *r)
{
	resampler_drop_input(r);
	r->frac = 0;
	memset(r->h, 0, sizeof(r->h));
}

static inline const int16_t *next_in(struct resampler *r)
{
	if (r->in_idx >= r->in_len) {
		r->refill(r->ctx, &r->in, &r->in_len);
		r->in_idx = 0;
	}
	return &r->in[2 * r->in_idx++];
}

static void shift_in(struct resampler *r)
{
	const int16_t *s = next_in(r);

	for (int c = 0; c < 2; c++) {
		r->h[0][c] = r->h[1][c];
		r->h[1][c] = r->h[2][c];
		r->h[2][c] = r->h[3][c];
		r->h[3][c] = (float)s[c];
	}
}

/* Noi suy Hermite (Catmull-Rom) giua x0 va x1 tai t trong [0, 1) */
static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
	const float c1 = 0.5f * (x1 - xm1);
	const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
	const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

	return ((c3 * t + c2) * t + c1) * t + x0;
}

void resampler_process(struct resampler *r, int16_t *dst, uint16_t n)
{
	if (r->num == r->den) {
		for (uint16_t i = 0; i < n; i++) {
			const int16_t *s = next_in(r);

			dst[2 * i] = s[0];
			dst[2 * i + 1] = s[1];
		}
		return;
	}

	for (uint16_t i = 0; i < n; i++) {
		const float t = (float)r->frac / (float)r->den;

		for (int c = 0; c < 2; c++) {
			const float y = hermite(r->h[0][c], r->h[1][c], r->h[2][c], r->h[3][c], t);

			dst[2 * i + c] = me54_sat16((int32_t)(y >= 0.0f ? y + 0.5f : y - 0.5f));
		}
		r->frac += r->num;
		while (r->frac >= r->den) {
			r->frac -= r->den;
			shift_in(r);
		}
	}
}

void resampler_up2(float h[4], const int16_t *in, int16_t *out, uint16_t n)
{
	for (uint16_t k = 0; k < n; k++) {
		float xm1;
		float x0;
		float x1;
		float x2;
		float y;

		h[0] = h[1];
		h[1] = h[2];
		h[2] = h[3];
		h[3] = (float)in[k];
		xm1 = h[0];
		x0 = h[1];
		x1 = h[2];
		x2 = h[3];

		/* Catmull-Rom tai t = 0.5: x0 + c1/2 + c2/4 + c3/8 */
		y = x0 + 0.5f * (0.5f * (x1 - xm1)) +
		    0.25f * (xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2) +
		    0.125f * (0.5f * (x2 - xm1) + 1.5f * (x0 - x1));
		out[2 * k] = (int16_t)x0;
		out[2 * k + 1] = (int16_t)ME54_CLAMP((int32_t)y, INT16_MIN, INT16_MAX);
	}
}
