/*
 * Helmet - bo tao am bao (song sin), dung trong thread audio (audio_out.c).
 *
 * Moi am bao la danh sach doan {tan so, thoi luong, khoang lang}. Song sin tao bang bo tich
 * luy pha 32 bit + bang 256 mau, noi suy tuyen tinh. Moi doan co doc len/xuong 5 ms de khong
 * keu "lup bup". Am bao co the lap lai (chuong) cho toi khi bi dung.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "helmet.h"

#define SINE_LEN  256
#define RAMP_MS   5

struct prompt_seg {
	uint16_t freq;   /* Hz, 0 = im lang */
	uint16_t ms;     /* thoi luong co tieng */
	uint16_t gap_ms; /* khoang lang sau doan */
};

struct prompt_def {
	const char *name;
	const struct prompt_seg *seg;
	uint8_t n;
	bool loop;
};

static const struct prompt_seg s_power_on[] = {{880, 80, 20}, {1175, 80, 20}, {1568, 120, 0}};
static const struct prompt_seg s_power_off[] = {{1568, 80, 20}, {1175, 80, 20}, {880, 160, 0}};
static const struct prompt_seg s_connected[] = {{1319, 60, 60}, {1319, 60, 0}};
static const struct prompt_seg s_disconnected[] = {{659, 200, 0}};
static const struct prompt_seg s_ring[] = {{1000, 400, 200}, {1000, 400, 2000}};
static const struct prompt_seg s_mic_muted[] = {{1047, 80, 10}, {523, 120, 0}};
static const struct prompt_seg s_mic_unmuted[] = {{523, 80, 10}, {1047, 120, 0}};
static const struct prompt_seg s_vol_limit[] = {{2093, 30, 0}};
static const struct prompt_seg s_pairing[] = {
	{1568, 50, 50}, {1568, 50, 50}, {1568, 50, 50}, {1568, 50, 0}};
static const struct prompt_seg s_temp_alarm[] = {{2637, 150, 100}, {2637, 150, 100}, {2637, 150, 0}};
static const struct prompt_seg s_low_battery[] = {{440, 150, 100}, {440, 150, 0}};
static const struct prompt_seg s_status_ok[] = {{1319, 80, 0}};
static const struct prompt_seg s_status_no_link[] = {{1319, 80, 80}, {1319, 80, 0}};
/* Intercom: master = cap tieng di len, slave = di xuong (lap toi khi co ket qua) */
static const struct prompt_seg s_ic_search[] = {{1175, 60, 40}, {1568, 90, 900}};
static const struct prompt_seg s_ic_wait[] = {{1568, 60, 40}, {1175, 90, 900}};
static const struct prompt_seg s_ic_linked[] = {{1047, 60, 30}, {1319, 60, 30}, {1568, 120, 0}};
static const struct prompt_seg s_ic_end[] = {{1568, 60, 30}, {1319, 60, 30}, {1047, 120, 0}};
static const struct prompt_seg s_ic_lost[] = {{784, 150, 50}, {587, 250, 0}};
static const struct prompt_seg s_ic_unavail[] = {{587, 100, 80}, {587, 100, 0}};

#define DEF(id, nm, s, lp) [id] = {.name = nm, .seg = s, .n = ARRAY_SIZE(s), .loop = lp}

static const struct prompt_def defs[PROMPT_COUNT] = {
	DEF(PROMPT_POWER_ON, "power_on", s_power_on, false),
	DEF(PROMPT_POWER_OFF, "power_off", s_power_off, false),
	DEF(PROMPT_CONNECTED, "connected", s_connected, false),
	DEF(PROMPT_DISCONNECTED, "disconnected", s_disconnected, false),
	DEF(PROMPT_RING, "ring", s_ring, true),
	DEF(PROMPT_MIC_MUTED, "mic_muted", s_mic_muted, false),
	DEF(PROMPT_MIC_UNMUTED, "mic_unmuted", s_mic_unmuted, false),
	DEF(PROMPT_VOL_LIMIT, "vol_limit", s_vol_limit, false),
	DEF(PROMPT_PAIRING, "pairing", s_pairing, false),
	DEF(PROMPT_TEMP_ALARM, "temp_alarm", s_temp_alarm, false),
	DEF(PROMPT_LOW_BATTERY, "low_battery", s_low_battery, false),
	DEF(PROMPT_STATUS_OK, "status_ok", s_status_ok, false),
	DEF(PROMPT_STATUS_NO_LINK, "status_no_link", s_status_no_link, false),
	DEF(PROMPT_IC_SEARCH, "ic_search", s_ic_search, true),
	DEF(PROMPT_IC_WAIT, "ic_wait", s_ic_wait, true),
	DEF(PROMPT_IC_LINKED, "ic_linked", s_ic_linked, false),
	DEF(PROMPT_IC_END, "ic_end", s_ic_end, false),
	DEF(PROMPT_IC_LOST, "ic_lost", s_ic_lost, false),
	DEF(PROMPT_IC_UNAVAIL, "ic_unavail", s_ic_unavail, false),
};

/* Bang sin 1 chu ky + 1 mau de noi suy khong phai quay vong */
static int16_t sine[SINE_LEN + 1];

void prompt_init_tables(void)
{
	for (int i = 0; i <= SINE_LEN; i++) {
		sine[i] = (int16_t)lroundf(32767.0f * sinf(2.0f * 3.14159265f * i / SINE_LEN));
	}
}

static void seg_setup(struct prompt_gen *g)
{
	const struct prompt_def *d = g->def;
	const struct prompt_seg *s = &d->seg[g->seg];

	g->pos = 0;
	g->tone = (uint32_t)s->ms * g->rate / 1000U;
	g->total = g->tone + (uint32_t)s->gap_ms * g->rate / 1000U;
	g->inc = (uint32_t)(((uint64_t)s->freq << 32) / g->rate);
	g->phase = 0;
}

void prompt_start(struct prompt_gen *g, enum helmet_prompt p, uint32_t rate)
{
	if ((int)p < 0 || p >= PROMPT_COUNT || rate == 0) {
		return;
	}
	g->def = &defs[p];
	g->id = (uint8_t)p;
	g->rate = rate;
	g->ramp = rate * RAMP_MS / 1000U;
	g->seg = 0;
	g->active = true;
	seg_setup(g);
}

void prompt_set_rate(struct prompt_gen *g, uint32_t rate)
{
	const struct prompt_def *d = g->def;

	if (!g->active || rate == g->rate || rate == 0) {
		return;
	}
	/* Giu vi tri tuong doi trong doan, doi buoc pha theo tan so moi */
	g->pos = (uint32_t)((uint64_t)g->pos * rate / g->rate);
	g->rate = rate;
	g->ramp = rate * RAMP_MS / 1000U;
	g->tone = (uint32_t)d->seg[g->seg].ms * rate / 1000U;
	g->total = g->tone + (uint32_t)d->seg[g->seg].gap_ms * rate / 1000U;
	g->inc = (uint32_t)(((uint64_t)d->seg[g->seg].freq << 32) / rate);
}

void prompt_stop(struct prompt_gen *g)
{
	g->active = false;
}

static int16_t sine_at(uint32_t phase)
{
	const uint32_t idx = phase >> 24;
	const int32_t frac = (int32_t)((phase >> 8) & 0xFFFF);
	const int32_t a = sine[idx];
	const int32_t b = sine[idx + 1];

	return (int16_t)(a + (((b - a) * frac) >> 16));
}

bool prompt_render(struct prompt_gen *g, int16_t *dst, uint16_t n, uint16_t amp)
{
	const struct prompt_def *d = g->def;

	for (uint16_t i = 0; i < n; i++) {
		int32_t v = 0;

		if (!g->active) {
			dst[i] = 0;
			continue;
		}

		if (g->pos < g->tone && d->seg[g->seg].freq != 0) {
			/* Doc len o dau doan, doc xuong o cuoi doan */
			uint32_t env = MIN(g->pos, g->tone - 1 - g->pos);
			int32_t a = amp;

			if (g->ramp > 0 && env < g->ramp) {
				a = (int32_t)((int64_t)amp * env / g->ramp);
			}
			v = ((int32_t)sine_at(g->phase) * a) >> 15;
			g->phase += g->inc;
		}
		dst[i] = (int16_t)v;

		if (++g->pos >= g->total) {
			if (++g->seg >= d->n) {
				if (!d->loop) {
					g->active = false;
					continue;
				}
				g->seg = 0;
			}
			seg_setup(g);
		}
	}

	return g->active;
}

const char *prompt_name(int p)
{
	return (p >= 0 && p < PROMPT_COUNT) ? defs[p].name : "?";
}

int prompt_from_name(const char *name)
{
	for (int i = 0; i < PROMPT_COUNT; i++) {
		if (strcmp(defs[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}
