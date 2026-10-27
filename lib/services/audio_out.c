/*
 * Lop DICH VU - duong xuat am thanh cua mu (the he 2)
 *
 *   SDU LC3 --audio_q--> audio thread: giai ma, ghep kenh L/R
 *     --> [1] VONG PCM (nhip dien thoai) --bu troi tung mau--> khung danh dinh 10 ms
 *         (core/pcm_ring.c)
 *     --> [2] am luong nhac + tron am bao (giam nhac -10 dB khi co am bao)
 *         (core/mixer.c, core/prompts.c)
 *     --> (diem lay tham chieu cho bo khu vong, mien 16 kHz)
 *     --> [3] lay mau lai ti le CO DINH (Hermite, core/resampler.c)
 *     --> I2S20 (tan so that, hal/i2s_out.c) --> 2 x MAX98357A
 *
 * - I2S cua nRF54L15 chi chia nguyen tu 32 MHz nen khong ra dung 48/32/24/16 kHz
 *   (vd 48 kHz -> 47 619.05 Hz). Ti le lay mau lai duoc tinh CHINH XAC tu chinh ham
 *   nrfx_i2s_prescalers_calc() ma driver dung (i2s_out_actual_rate()), dang phan so (vd 126/125
 *   mau vao / mau ra), bo dem pha la so nguyen nen khong bao gio tich luy sai so.
 * - Lech clock dien thoai (vai chuc ppm) bu bang cach lay them / bot 1 mau trong mot khung
 *   (noi suy tuyen tinh) theo muc day TRUNG BINH cua vong PCM - tai khong phan biet duoc.
 * - Dau ra duoc "bom" theo muc day hang doi I2S (thread thuc moi 3 ms khi dang phat), khong
 *   phu thuoc nhip SDU -> am bao phat duoc ca khi khong co luong nao.
 * - Tham chieu AEC lay sau [2]: tu do toi loa chi con [3] (ti le co dinh, cung thach anh voi
 *   micro) nen do tre tham chieu -> micro la hang so suot cuoc goi.
 * - Moi thao tac giai ma (FPU) chi chay trong audio thread.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lc3.h>

#include <me54/core/mixer.h>
#include <me54/core/pcm_ring.h>
#include <me54/core/prompts.h>
#include <me54/core/resampler.h>
#include <me54/hal/i2s_out.h>
#include <me54/services/audio_out.h>

LOG_MODULE_REGISTER(audio_out, LOG_LEVEL_INF);

#define MAX_FREQ_HZ       48000
#define MAX_FRAME_US      10000
#define MAX_FRAME_SAMPLES (MAX_FREQ_HZ * MAX_FRAME_US / USEC_PER_SEC) /* 480 */
#define I2S_BLOCK_MAX     (MAX_FRAME_SAMPLES * 2 * sizeof(int16_t))    /* 1920 B */

/* Giu 4 khoi (~40 ms) da cap cho I2S: 2 khoi phan cung dang giu (dang phat + ke tiep) +
 * 2 khoi trong hang doi driver (CONFIG_I2S_NRFX_TX_BLOCK_COUNT = 4) -> thread nay duoc phep
 * cham ~20 ms truoc khi I2S can ("Next buffers not supplied on time"). 3 khoi chi cho ~10 ms:
 * khong du khi SDU broadcast den don cuc (do 19/10/2026: tach 1-3 lan/s).
 */
#define I2S_TARGET_BLOCKS 4
#define I2S_START_BLOCKS  2 /* bat dau phat khi da nap 2 khoi */
#define PUMP_PERIOD_MS    3

BUILD_ASSERT(I2S_OUT_BLOCK_BYTES >= I2S_BLOCK_MAX, "CONFIG_ME54_HAL_I2S_OUT_BLOCK_BYTES < 1920");
BUILD_ASSERT(I2S_OUT_BLOCK_COUNT > I2S_TARGET_BLOCKS, "CONFIG_ME54_HAL_I2S_OUT_BLOCK_COUNT too small");

#define RING_CAP    (MAX_FRAME_SAMPLES * 5) /* mau stereo: 50 ms o 48 kHz */
#define TAIL_FRAMES 3                       /* khung im lang truoc khi dung I2S */

#define PROMPT_RATE    16000
#define PROMPT_AMP     4125  /* -18 dBFS */
#define PROMPT_AMP_MIN 1036  /* -30 dBFS: chuong khong nho hon muc nay */
#define DUCK_Q15       10362 /* -10 dB */

#define REF_LEN  2048 /* 128 ms o 16 kHz (luy thua 2) */
#define REF_MASK (REF_LEN - 1)

#define CH_L BIT(0)
#define CH_R BIT(1)

#define AUDIO_THREAD_STACK 6144
#define AUDIO_THREAD_PRIO  K_PRIO_PREEMPT(2)

enum audio_msg_type {
	MSG_START,
	MSG_STOP,
	MSG_DATA,
	MSG_PROMPT,
	MSG_PROMPT_STOP,
	MSG_SHUTDOWN,
};

struct audio_msg {
	uint8_t type;
	uint8_t idx;
	uint8_t valid;
	uint16_t len;
	uint32_t ts;
	union {
		struct audio_stream_cfg cfg;
		uint8_t data[AUDIO_STREAM_MAX_SDU];
	};
};

K_MSGQ_DEFINE(audio_q, sizeof(struct audio_msg), 10, 4);
static K_SEM_DEFINE(shutdown_sem, 0, 1);

/* ---------------- Giai ma tung luong ---------------- */

struct dec_stream {
	bool active;
	struct audio_stream_cfg cfg;
	uint8_t out_mask[2]; /* kenh LC3 thu c -> loa CH_L / CH_R / ca hai */
	lc3_decoder_t dec[2];
	lc3_decoder_mem_48k_t mem[2];
};

static struct dec_stream ds[AUDIO_OUT_STREAMS];

static int16_t frame_pcm[MAX_FRAME_SAMPLES * 2]; /* khung dang ghep (L, R xen ke) */
static uint8_t frame_mask;
static uint8_t expected_mask;
static uint32_t frame_ts;
static uint16_t stream_samples;

/* ---------------- [1] Vong PCM ---------------- */

static int16_t ring_buf[RING_CAP * 2];
static struct pcm_ring ring = {.buf = ring_buf, .cap = RING_CAP}; /* ring.on: dang lay tu vong */

/* ---------------- Dau ra ---------------- */

static uint32_t nom_rate;  /* tan so danh dinh dang cau hinh */
static uint16_t nom_frame; /* mau / khung danh dinh */
static int16_t nom_buf[MAX_FRAME_SAMPLES * 2];
static int16_t prompt_buf[MAX_FRAME_SAMPLES];
static bool i2s_running;
static bool out_active;
static bool is_shutdown;
static uint8_t tail;

/* [3] Lay mau lai: moi mau ra tieu thu num / den mau vao; lay khung danh dinh qua refill */
static void refill_nominal(void *ctx, const int16_t **buf, uint16_t *len);
static struct resampler rs = {.num = 1, .den = 1, .refill = refill_nominal};

/* Am bao */
static struct prompt_gen pg;
static atomic_t prompt_busy;

/* Tham chieu cho bo khu vong (mien 16 kHz, mono) */
static int16_t ref_ring[REF_LEN];
static uint32_t ref_wr;
static uint32_t ref_anchor_pos;
static uint32_t ref_anchor_cyc;
static uint32_t ref_gen;
static bool ref_anchor_ok;
static uint32_t ref_start_pos;
static struct k_spinlock ref_lock;

/* He so am luong Q15 (32767 = 0 dB) */
static atomic_t gain_q15 = ATOMIC_INIT(32767);

static struct audio_out_stats stats;
static uint32_t i2s_rate_mhz;

/* Do hieu nang (chu ky k_cycle_get_32) */
struct perf {
	uint32_t sum;
	uint32_t n;
	uint32_t max;
};
static struct perf perf_dec;
static struct perf perf_out;

static void perf_add(struct perf *p, uint32_t cyc)
{
	p->sum += cyc;
	p->n++;
	p->max = MAX(p->max, cyc);
}

/* ---------------- I2S ---------------- */

/* Dung I2S ngay lap tuc va bo du lieu dang cho */
static void i2s_halt(void)
{
	if (!i2s_out_configured()) {
		return;
	}
	i2s_out_halt();
	i2s_running = false;
	out_active = false;
	ref_anchor_ok = false;
}

/* Tan so I2S that (cung thuat toan driver dung) -> ti le lay mau lai */
static void i2s_calc_resample(uint32_t rate)
{
	struct i2s_out_rate r;
	const int err = i2s_out_actual_rate(rate, &r);

	resampler_set_ratio(&rs, r.num, r.den);
	i2s_rate_mhz = r.actual_mhz;

	switch (err) {
	case 0:
		LOG_INF("I2S %u Hz -> actual %u.%03u Hz (MCK 32 MHz/%u, ratio %u), resample %u/%u",
			rate, i2s_rate_mhz / 1000U, i2s_rate_mhz % 1000U, r.mck_div, r.ratio, r.num,
			r.den);
		break;
	case -EIO:
		LOG_WRN("I2S %u Hz: cannot compute prescalers - no resampling", rate);
		break;
	case -ENOTSUP:
		LOG_WRN("I2S %u Hz: unknown clock setup - no resampling", rate);
		break;
	default:
		LOG_WRN("I2S %u Hz: actual rate too far off - no resampling", rate);
		break;
	}
}

static int i2s_setup(uint32_t rate, uint16_t samples)
{
	int err;

	i2s_halt();

	err = i2s_out_configure(rate, samples, 20);
	if (err) {
		LOG_ERR("i2s_configure %u Hz failed (%d)", rate, err);
		return err;
	}

	nom_rate = rate;
	nom_frame = samples;
	resampler_drop_input(&rs);
	i2s_calc_resample(rate);
	prompt_set_rate(&pg, rate);
	return 0;
}

/* Dam bao I2S dang o tan so 'rate' (cau hinh lai neu khac) */
static void ensure_rate(uint32_t rate, uint16_t samples)
{
	if (!i2s_out_configured() || nom_rate != rate || nom_frame != samples) {
		(void)i2s_setup(rate, samples);
	}
}

/* ---------------- Tham chieu AEC ---------------- */

static void ref_write(const int16_t *f, uint16_t n)
{
	k_spinlock_key_t key;

	if (nom_rate != 16000 && nom_rate != 32000) {
		return; /* chi co cuoc goi (16/32 kHz) moi can tham chieu */
	}

	key = k_spin_lock(&ref_lock);
	if (nom_rate == 16000) {
		for (uint16_t i = 0; i < n; i++) {
			ref_ring[ref_wr++ & REF_MASK] = (int16_t)((f[2 * i] + f[2 * i + 1]) / 2);
		}
	} else {
		/* 32 kHz -> 16 kHz: trung binh tung cap mau */
		for (uint16_t i = 0; i + 1 < n; i += 2) {
			const int32_t m = f[2 * i] + f[2 * i + 1] + f[2 * i + 2] + f[2 * i + 3];

			ref_ring[ref_wr++ & REF_MASK] = (int16_t)(m / 4);
		}
	}
	k_spin_unlock(&ref_lock, key);
}

bool audio_out_ref_anchor(uint32_t *ref_pos, uint32_t *t_cyc, uint32_t *gen)
{
	k_spinlock_key_t key = k_spin_lock(&ref_lock);
	const bool ok = ref_anchor_ok;

	*ref_pos = ref_anchor_pos;
	*t_cyc = ref_anchor_cyc;
	*gen = ref_gen;
	k_spin_unlock(&ref_lock, key);
	return ok;
}

int audio_out_ref_read(int16_t *dst, uint16_t n, uint32_t pos)
{
	k_spinlock_key_t key = k_spin_lock(&ref_lock);
	int count = 0;

	for (uint16_t i = 0; i < n; i++) {
		const int32_t age = (int32_t)(ref_wr - (pos + i));

		if (age > 0 && age <= REF_LEN) {
			dst[i] = ref_ring[(pos + i) & REF_MASK];
			count++;
		} else {
			dst[i] = 0;
		}
	}
	k_spin_unlock(&ref_lock, key);
	return count;
}

/* ---------------- [2] Khung danh dinh: nhac + am luong + am bao ---------------- */

static void build_nominal_frame(void)
{
	const uint16_t n = nom_frame;
	bool music = false;

	if (expected_mask != 0 && ring.on) {
		const uint16_t take = pcm_ring_slip_take(&ring, n);

		if (pcm_ring_count(&ring) >= take) {
			pcm_ring_pull(&ring, nom_buf, n, take);
			music = true;
			if (take > n) {
				stats.slip_drop++;
			} else if (take < n) {
				stats.slip_insert++;
			}
		} else {
			stats.underrun++;
			memset(nom_buf, 0, n * 2 * sizeof(int16_t));
		}
	} else {
		memset(nom_buf, 0, n * 2 * sizeof(int16_t));
	}

	if (music) {
		int32_t g = (int32_t)atomic_get(&gain_q15);

		if (pg.active) {
			g = (g * DUCK_Q15) >> 15;
		}
		mixer_gain_q15(nom_buf, 2U * n, g);
	}

	if (pg.active) {
		uint16_t amp = PROMPT_AMP;
		bool still;

		if (pg.id == PROMPT_RING) {
			/* Chuong theo am luong, nhung khong nho hon -30 dBFS */
			amp = (uint16_t)MAX(PROMPT_AMP_MIN,
					    ((int32_t)PROMPT_AMP * (int32_t)atomic_get(&gain_q15)) >> 15);
		}
		still = prompt_render(&pg, prompt_buf, n, amp);
		mixer_add_mono(nom_buf, prompt_buf, n);
		if (!still) {
			atomic_clear(&prompt_busy);
		}
	}

	ref_write(nom_buf, n);
}

/* ---------------- [3] Lay mau lai co dinh ---------------- */

static void refill_nominal(void *ctx, const int16_t **buf, uint16_t *len)
{
	ARG_UNUSED(ctx);
	build_nominal_frame();
	*buf = nom_buf;
	*len = nom_frame;
}

static void produce_block(int16_t *dst, uint16_t n)
{
	resampler_process(&rs, dst, n);
}

/* ---------------- Bom du lieu ra I2S ---------------- */

static void out_start(void)
{
	k_spinlock_key_t key;

	out_active = true;
	tail = 0;
	resampler_reset(&rs);

	key = k_spin_lock(&ref_lock);
	ref_start_pos = ref_wr; /* mau tham chieu dau tien cua lan phat nay */
	k_spin_unlock(&ref_lock, key);
}

static void out_drain_stop(void)
{
	(void)i2s_out_drain();
	i2s_running = false;
	out_active = false;
	ref_anchor_ok = false;
	if (!pg.active) {
		atomic_clear(&prompt_busy);
	}
}

static void pump(void)
{
	const size_t size = nom_frame * 2 * sizeof(int16_t);

	if (!i2s_out_configured() || is_shutdown) {
		return;
	}

	if (!out_active) {
		if (!pg.active && !(expected_mask != 0 && ring.on)) {
			return;
		}
		out_start();
	}

	while (i2s_out_blocks_in_use() < I2S_TARGET_BLOCKS) {
		uint32_t t0;
		void *blk;
		int err;

		if (!pg.active && expected_mask == 0) {
			if (tail >= TAIL_FRAMES) {
				out_drain_stop();
				return;
			}
			tail++;
		} else {
			tail = 0;
		}

		if (i2s_out_alloc(&blk, K_NO_WAIT) != 0) {
			break;
		}

		t0 = k_cycle_get_32();
		produce_block(blk, nom_frame);
		perf_add(&perf_out, k_cycle_get_32() - t0);

		err = i2s_out_write(blk, size);
		if (err) {
			i2s_out_free(blk);
			stats.i2s_err++;
			LOG_WRN("I2S write failed (%d) - restarting output", err);
			i2s_halt();
			return;
		}

		if (!i2s_running && i2s_out_blocks_in_use() >= I2S_START_BLOCKS) {
			k_spinlock_key_t key;

			err = i2s_out_start();
			if (err) {
				stats.i2s_err++;
				LOG_WRN("I2S start failed (%d)", err);
				i2s_halt();
				return;
			}
			i2s_running = true;

			key = k_spin_lock(&ref_lock);
			ref_anchor_pos = ref_start_pos;
			ref_anchor_cyc = k_cycle_get_32();
			ref_anchor_ok = (nom_rate == 16000 || nom_rate == 32000);
			ref_gen++;
			k_spin_unlock(&ref_lock, key);
			LOG_DBG("I2S output running (%u Hz)", nom_rate);
		}
	}
}

/* ---------------- Ghep khung tu cac luong ---------------- */

static void flush_frame(void)
{
	if (frame_mask == 0) {
		return;
	}

	/* Thieu mot ben -> chep ben con lai sang (mu co 2 loa, luon phat ca hai) */
	for (uint16_t i = 0; i < stream_samples; i++) {
		if (!(frame_mask & CH_L)) {
			frame_pcm[2 * i] = frame_pcm[2 * i + 1];
		} else if (!(frame_mask & CH_R)) {
			frame_pcm[2 * i + 1] = frame_pcm[2 * i];
		}
	}

	if (pcm_ring_push(&ring, frame_pcm, stream_samples, (uint32_t)nom_frame * 3U / 2U)) {
		stats.ring_drop++;
	}
	frame_mask = 0;
}

static void update_expected_mask(void)
{
	expected_mask = 0;
	for (size_t i = 0; i < ARRAY_SIZE(ds); i++) {
		if (ds[i].active) {
			for (uint8_t c = 0; c < ds[i].cfg.chan_cnt; c++) {
				expected_mask |= ds[i].out_mask[c];
			}
		}
	}
}

/* Kenh LC3 -> loa: theo Audio Location ma dien thoai gan cho luong */
static void map_channels(struct dec_stream *d)
{
	const uint32_t alloc = d->cfg.chan_alloc;

	if (d->cfg.chan_cnt >= 2) {
		/* Thu tu kenh trong SDU theo bit location tang dan: FL truoc FR */
		d->out_mask[0] = CH_L;
		d->out_mask[1] = CH_R;
	} else if ((alloc & BT_AUDIO_LOCATION_FRONT_LEFT) &&
		   !(alloc & BT_AUDIO_LOCATION_FRONT_RIGHT)) {
		d->out_mask[0] = CH_L;
	} else if ((alloc & BT_AUDIO_LOCATION_FRONT_RIGHT) &&
		   !(alloc & BT_AUDIO_LOCATION_FRONT_LEFT)) {
		d->out_mask[0] = CH_R;
	} else {
		/* Mono (alloc = 0) hoac vi tri khac -> phat ra ca hai loa */
		d->out_mask[0] = CH_L | CH_R;
	}
}

/* ---------------- Xu ly thong diep ---------------- */

static void handle_start(uint8_t idx, const struct audio_stream_cfg *cfg)
{
	struct dec_stream *d = &ds[idx];
	const int samples = lc3_frame_samples(cfg->frame_dur_us, cfg->freq_hz);

	if (is_shutdown) {
		return;
	}
	if (samples <= 0 || samples > MAX_FRAME_SAMPLES || cfg->chan_cnt == 0 ||
	    cfg->chan_cnt > 2) {
		LOG_ERR("Stream %u: unsupported config (%u Hz, %u us, %u ch)", idx, cfg->freq_hz,
			cfg->frame_dur_us, cfg->chan_cnt);
		return;
	}
	if (expected_mask != 0 && (cfg->freq_hz != nom_rate || samples != stream_samples)) {
		LOG_ERR("Stream %u: %u Hz differs from active stream (%u Hz)", idx, cfg->freq_hz,
			nom_rate);
		return;
	}

	d->cfg = *cfg;
	for (uint8_t c = 0; c < cfg->chan_cnt; c++) {
		d->dec[c] = lc3_setup_decoder(cfg->frame_dur_us, cfg->freq_hz, 0, &d->mem[c]);
		if (d->dec[c] == NULL) {
			LOG_ERR("Stream %u: LC3 decoder setup failed", idx);
			d->active = false;
			return;
		}
	}
	map_channels(d);

	if (expected_mask == 0) {
		pcm_ring_reset(&ring);
		frame_mask = 0;
	}
	d->active = true;
	update_expected_mask();
	stream_samples = (uint16_t)samples;
	ensure_rate(cfg->freq_hz, (uint16_t)samples);

	LOG_INF("Stream %u decoding: %u Hz, %u us, %u ch, %u B/frame, alloc 0x%x -> %s", idx,
		cfg->freq_hz, cfg->frame_dur_us, cfg->chan_cnt, cfg->octets_per_frame,
		cfg->chan_alloc,
		cfg->chan_cnt >= 2                  ? "L+R"
		: d->out_mask[0] == CH_L            ? "L"
		: d->out_mask[0] == CH_R            ? "R"
						    : "L=R (mono)");
}

static void handle_stop(uint8_t idx)
{
	if (!ds[idx].active) {
		return;
	}

	ds[idx].active = false;
	update_expected_mask();
	LOG_INF("Stream %u stopped", idx);

	if (expected_mask == 0) {
		pcm_ring_reset(&ring);
		frame_mask = 0;
		if (!pg.active) {
			i2s_halt();
			LOG_INF("I2S output stopped");
		}
	}
}

static void handle_data(struct audio_msg *m)
{
	struct dec_stream *d = &ds[m->idx];
	const uint16_t octets = d->cfg.octets_per_frame;
	bool valid = m->valid;
	uint32_t t0;

	if (!d->active || !i2s_out_configured()) {
		return;
	}

	stats.sdu_rx++;
	if (!valid) {
		stats.sdu_bad++;
	} else if (m->len < octets * d->cfg.chan_cnt) {
		stats.len_err++;
		valid = false;
	}

	/* SDU cua khoang thoi gian moi -> day khung cu ra truoc (vd luong kia bi mat) */
	if (frame_mask != 0 && m->ts != frame_ts) {
		flush_frame();
	}
	frame_ts = m->ts;

	t0 = k_cycle_get_32();
	for (uint8_t c = 0; c < d->cfg.chan_cnt; c++) {
		const uint8_t mask = d->out_mask[c];
		int16_t *dst = (mask & CH_L) ? &frame_pcm[0] : &frame_pcm[1];
		const void *in = valid ? &m->data[c * octets] : NULL; /* NULL -> PLC */
		const int ret = lc3_decode(d->dec[c], in, octets, LC3_PCM_FORMAT_S16, dst, 2);

		if (ret == 1) {
			stats.plc++;
		} else if (ret < 0) {
			stats.dec_err++;
			for (uint16_t i = 0; i < stream_samples; i++) {
				dst[2 * i] = 0;
			}
		}

		if (mask == (CH_L | CH_R)) {
			for (uint16_t i = 0; i < stream_samples; i++) {
				frame_pcm[2 * i + 1] = frame_pcm[2 * i];
			}
		}
		frame_mask |= mask;
	}
	perf_add(&perf_dec, k_cycle_get_32() - t0);

	if ((frame_mask & expected_mask) == expected_mask) {
		flush_frame();
	}
}

static void handle_prompt(uint8_t id)
{
	if (expected_mask == 0 && !out_active) {
		/* Khong co luong nao: tu cau hinh I2S cho am bao */
		ensure_rate(PROMPT_RATE, PROMPT_RATE / 100U);
	}
	if (!i2s_out_configured()) {
		atomic_clear(&prompt_busy);
		return;
	}
	prompt_start(&pg, (enum prompt_id)id, nom_rate);
	LOG_DBG("Prompt %s", prompt_name(id));
}

static void handle_shutdown(void)
{
	i2s_halt();
	for (size_t i = 0; i < ARRAY_SIZE(ds); i++) {
		ds[i].active = false;
	}
	expected_mask = 0;
	pcm_ring_reset(&ring);
	prompt_stop(&pg);
	atomic_clear(&prompt_busy);
	is_shutdown = true;
	k_sem_give(&shutdown_sem);
}

static void handle_msg(struct audio_msg *msg)
{
	switch (msg->type) {
	case MSG_START:
		handle_start(msg->idx, &msg->cfg);
		break;
	case MSG_STOP:
		handle_stop(msg->idx);
		break;
	case MSG_DATA:
		handle_data(msg);
		break;
	case MSG_PROMPT:
		handle_prompt(msg->idx);
		break;
	case MSG_PROMPT_STOP:
		prompt_stop(&pg);
		atomic_clear(&prompt_busy);
		break;
	case MSG_SHUTDOWN:
		handle_shutdown();
		break;
	default:
		break;
	}
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	static struct audio_msg msg;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		const bool busy = out_active || pg.active || (expected_mask != 0 && ring.on);

		if (k_msgq_get(&audio_q, &msg, busy ? K_MSEC(PUMP_PERIOD_MS) : K_FOREVER) == 0) {
			do {
				handle_msg(&msg);
				/* Nap I2S sau MOI SDU: nhieu SDU den don cuc (broadcast BN > 1, 2 BIS)
				 * giai ma lien nhau toi ~17 ms se lam I2S can. pump() rat nhe khi du khoi.
				 */
				pump();
			} while (k_msgq_get(&audio_q, &msg, K_NO_WAIT) == 0);
		}
		pump();
	}
}

K_THREAD_DEFINE(audio_tid, AUDIO_THREAD_STACK, audio_thread, NULL, NULL, NULL, AUDIO_THREAD_PRIO,
		K_FP_REGS, 0);

/* ---------------- API cho cac module khac ---------------- */

int audio_out_init(void)
{
	prompt_init_tables();

	if (i2s_out_init() != 0) {
		LOG_ERR("I2S20 not ready");
		return -ENODEV;
	}
	LOG_INF("Audio out ready: LC3 -> clock slip -> prompts -> fixed resampler -> I2S20");
	return 0;
}

static void post_ctrl(uint8_t type, uint8_t idx, const struct audio_stream_cfg *cfg)
{
	struct audio_msg msg = {.type = type, .idx = idx};

	if (cfg != NULL) {
		msg.cfg = *cfg;
	}
	if (k_msgq_put(&audio_q, &msg, K_MSEC(100)) != 0) {
		LOG_ERR("Audio control message %u lost", type);
	}
}

void audio_out_stream_start(uint8_t idx, const struct audio_stream_cfg *cfg)
{
	if (idx < ARRAY_SIZE(ds)) {
		post_ctrl(MSG_START, idx, cfg);
	}
}

void audio_out_stream_stop(uint8_t idx)
{
	if (idx < ARRAY_SIZE(ds)) {
		post_ctrl(MSG_STOP, idx, NULL);
	}
}

/* Goi tu BT RX thread (thread duy nhat goi ham nay): chi copy du lieu, khong giai ma */
void audio_out_stream_data(uint8_t idx, bool valid, uint32_t ts, const uint8_t *data,
			   uint16_t len)
{
	static struct audio_msg msg;

	if (idx >= ARRAY_SIZE(ds)) {
		return;
	}
	if (len > AUDIO_STREAM_MAX_SDU) {
		stats.len_err++;
		valid = false;
		len = 0;
	}

	msg.type = MSG_DATA;
	msg.idx = idx;
	msg.valid = valid;
	msg.ts = ts;
	msg.len = len;
	if (valid && len > 0) {
		memcpy(msg.data, data, len);
	}

	if (k_msgq_put(&audio_q, &msg, K_NO_WAIT) != 0) {
		stats.q_full++;
	}
}

void audio_out_set_volume(uint8_t volume, bool mute)
{
	/* Duong cong binh phuong: gan voi cam nhan tai nguoi hon tuyen tinh */
	const uint32_t g = mute ? 0 : ((uint32_t)volume * volume * 32767U) / (255U * 255U);

	atomic_set(&gain_q15, (atomic_val_t)g);
}

void audio_out_prompt(enum prompt_id p)
{
	if ((int)p < 0 || p >= PROMPT_COUNT) {
		return;
	}
	atomic_set(&prompt_busy, 1);
	post_ctrl(MSG_PROMPT, (uint8_t)p, NULL);
}

void audio_out_prompt_stop(void)
{
	post_ctrl(MSG_PROMPT_STOP, 0, NULL);
}

bool audio_out_prompt_busy(void)
{
	return atomic_get(&prompt_busy) != 0;
}

void audio_out_shutdown(void)
{
	post_ctrl(MSG_SHUTDOWN, 0, NULL);
	(void)k_sem_take(&shutdown_sem, K_MSEC(300));
}

void audio_out_get_stats(struct audio_out_stats *out)
{
	*out = stats;
	out->out_rate = nom_rate;
	out->i2s_rate_mhz = i2s_rate_mhz;
	out->frame_samples = nom_frame;
	out->ring_level = (uint16_t)MIN(pcm_ring_count(&ring), UINT16_MAX);
	out->active_mask = expected_mask;
	out->i2s_running = i2s_running;
	out->prompt_active = pg.active;
	out->queued_blocks = i2s_out_blocks_in_use();
}

static void print_perf(const struct shell *sh, const char *name, const struct perf *p,
		       uint32_t budget_us)
{
	const uint32_t avg = p->n ? p->sum / p->n : 0;
	const uint32_t avg_us = k_cyc_to_us_floor32(avg);
	const uint32_t max_us = k_cyc_to_us_floor32(p->max);

	shell_print(sh, "%-16s: avg %u us, max %u us (%u%% of %u us), n %u", name, avg_us, max_us,
		    budget_us ? avg_us * 100U / budget_us : 0, budget_us, p->n);
}

void audio_out_print_perf(const struct shell *sh)
{
	print_perf(sh, "LC3 decode/SDU", &perf_dec, 10000);
	print_perf(sh, "Out block", &perf_out, 10000);
}
