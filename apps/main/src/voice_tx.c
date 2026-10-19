/*
 * Helmet - chieu len cua cuoc goi: micro -> DSP -> bu troi clock -> LC3 -> SDU -> dien thoai.
 *
 *   voice thread (preempt 3):
 *     mic_in_read (10 ms, clock mu) -> doc tham chieu da can -> vdsp_process
 *     -> (tat micro: 0) -> VONG PCM -> lay 160 +/- 1 mau (bu troi) -> (32 kHz: nang x2)
 *     -> lc3_encode -> net_buf -> HANG SDU (toi da 4)
 *   bt_rx (callback sent, nhip CIS do dien thoai quyet dinh):
 *     lay 1 SDU khoi hang -> bt_bap_stream_send. Hang rong -> gui khung im lang.
 *
 * - Moi lan bat dau: gui ngay 2 SDU im lang de controller luon co du lieu, sau do moi `sent`
 *   gui 1 SDU -> nhip gui tu khoa theo nhip CIS.
 * - Clock micro (mu) va nhip CIS (dien thoai) lech toi ~50 ppm: muc day TRUNG BINH (vong PCM +
 *   hang SDU) lech khoi muc tieu thi lay 161 hoac 159 mau cho mot khung (noi suy ve 160).
 * - Tham chieu AEC: mau micro thu m ung voi mau tham chieu
 *       anchor_pos + (t_mic0 - t_anchor) * 16 kHz + m - AEC_MARGIN
 *   (xem audio_out.c). Tinh lai moi khi dau ra loa khoi dong lai (anchor gen doi).
 * - Doc micro loi MIC_RESTART_AFTER lan lien tiep (driver PDM tu dung khi hang doi day) ->
 *   khoi dong lai micro, dem lai vi tri mau va can lai tham chieu AEC.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include <lc3.h>

#include "helmet.h"
#include "voice_dsp.h"

LOG_MODULE_REGISTER(voice, LOG_LEVEL_INF);

#define SDU_MAX       80 /* 32_2: 80 octet */
#define TX_POOL_COUNT 6
#define SDU_Q_LEN     4
#define PRIME_SDUS    2
#define PCM_RING      640 /* 40 ms o 16 kHz */
#define LEVEL_SETTLE  50  /* khung truoc khi chot muc tieu (~0.5 s) */
#define LEVEL_HYST    24
#define AEC_MARGIN    32  /* lay tham chieu som 2 ms: duong am + tre bo loc PDM nam trong 256 he so */
#define SENT_WATCHDOG_MS 60
#define MIC_RESTART_AFTER 3 /* lan doc loi lien tiep (moi lan cho 30 ms) truoc khi chay lai */

#define VOICE_STACK 6144
#define VOICE_PRIO  K_PRIO_PREEMPT(3)

NET_BUF_POOL_FIXED_DEFINE(voice_pool, TX_POOL_COUNT, BT_ISO_SDU_BUF_SIZE(SDU_MAX),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

typedef LC3_ENCODER_MEM_T(10000, 32000) voice_enc_mem_t;

static K_SEM_DEFINE(start_sem, 0, 1);
static atomic_t session;      /* tang moi lan voice_start / voice_stop */
static atomic_t active;
static atomic_t muted;
static atomic_t tx_seq;
static atomic_t last_sent_cyc;
static atomic_t tone_hz; /* thu nghiem: thay giong noi bang am sin (0 = tat) */

static struct bt_bap_stream *tx_stream;
static struct helmet_stream_cfg tx_cfg;

static struct k_spinlock q_lock;
static struct net_buf *sdu_q[SDU_Q_LEN];
static uint8_t q_head;
static uint8_t q_count;

static voice_enc_mem_t enc_mem;
static lc3_encoder_t enc;
static uint8_t silence_sdu[SDU_MAX];

static int16_t pcm_ring[PCM_RING];
static uint32_t pr_rd;
static uint32_t pr_wr;
static int32_t level_avg_x16;
static int32_t level_target;
static uint16_t settle;
static float up_h[4]; /* lich su cho nang tan so x2 */

static struct helmet_voice_stats st;

struct perf {
	uint32_t sum;
	uint32_t n;
	uint32_t max;
};
static struct perf perf_dsp;
static struct perf perf_enc;

static void perf_add(struct perf *p, uint32_t cyc)
{
	p->sum += cyc;
	p->n++;
	p->max = MAX(p->max, cyc);
}

/* ---------------- Hang SDU (voice thread day vao, bt_rx lay ra) ---------------- */

static void q_push(struct net_buf *buf)
{
	k_spinlock_key_t key = k_spin_lock(&q_lock);
	struct net_buf *drop = NULL;

	if (q_count == SDU_Q_LEN) {
		drop = sdu_q[q_head];
		q_head = (q_head + 1) % SDU_Q_LEN;
		q_count--;
		st.q_drop++;
	}
	sdu_q[(q_head + q_count) % SDU_Q_LEN] = buf;
	q_count++;
	k_spin_unlock(&q_lock, key);

	if (drop != NULL) {
		net_buf_unref(drop);
	}
}

static struct net_buf *q_pop(void)
{
	k_spinlock_key_t key = k_spin_lock(&q_lock);
	struct net_buf *buf = NULL;

	if (q_count > 0) {
		buf = sdu_q[q_head];
		q_head = (q_head + 1) % SDU_Q_LEN;
		q_count--;
	}
	k_spin_unlock(&q_lock, key);
	return buf;
}

static uint8_t q_level(void)
{
	return q_count;
}

static void q_flush(void)
{
	struct net_buf *buf;

	while ((buf = q_pop()) != NULL) {
		net_buf_unref(buf);
	}
}

/* ---------------- Gui ---------------- */

static struct net_buf *make_sdu(const uint8_t *data, uint16_t len)
{
	struct net_buf *buf = net_buf_alloc(&voice_pool, K_NO_WAIT);

	if (buf == NULL) {
		st.pool_empty++;
		return NULL;
	}
	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, data, len);
	return buf;
}

static void send_buf(struct net_buf *buf)
{
	struct bt_bap_stream *stream = tx_stream;
	const uint16_t seq = (uint16_t)atomic_inc(&tx_seq);
	int err;

	if (stream == NULL) {
		net_buf_unref(buf);
		return;
	}
	err = bt_bap_stream_send(stream, buf, seq);
	if (err) {
		net_buf_unref(buf);
		st.send_err++;
		if (st.send_err < 5 || (st.send_err % 100) == 0) {
			LOG_WRN("SDU send failed (%d), %u errors", err, st.send_err);
		}
		return;
	}
	st.sdu_sent++;
	atomic_set(&last_sent_cyc, (atomic_val_t)k_cycle_get_32());
}

static void send_silence(void)
{
	struct net_buf *buf = make_sdu(silence_sdu, tx_cfg.octets_per_frame);

	if (buf != NULL) {
		send_buf(buf);
	}
}

void voice_on_sent(struct bt_bap_stream *stream)
{
	struct net_buf *buf;

	if (!atomic_get(&active) || stream != tx_stream) {
		return;
	}

	buf = q_pop();
	if (buf == NULL) {
		st.sdu_underrun++;
		send_silence();
		return;
	}
	send_buf(buf);
}

/* ---------------- Vong PCM + bu troi ---------------- */

static inline uint32_t pr_count(void)
{
	return pr_wr - pr_rd;
}

static void pcm_push(const int16_t *x, uint16_t n)
{
	if (pr_count() + n > PCM_RING) {
		pr_rd += n; /* khong nen xay ra: hang SDU da bo bot */
		st.q_drop++;
	}
	for (uint16_t i = 0; i < n; i++) {
		pcm_ring[(pr_wr + i) % PCM_RING] = x[i];
	}
	pr_wr += n;
}

/* Muc day (mau) = vong PCM + SDU dang cho; cap nhat trung binh 1 lan moi khoi micro */
static void level_update(void)
{
	const int32_t level = (int32_t)pr_count() + (int32_t)q_level() * VOICE_FRAME;

	if (settle == 0) {
		level_avg_x16 = level << 4;
	}
	level_avg_x16 += level - (level_avg_x16 >> 4);
	if (settle < LEVEL_SETTLE && ++settle == LEVEL_SETTLE) {
		level_target = level_avg_x16 >> 4;
	}
}

/* So mau lay cho khung ke tiep: 161 (bo 1), 160, hoac 159 (chen 1) */
static uint16_t frame_take(void)
{
	const int32_t avg = level_avg_x16 >> 4;

	if (settle < LEVEL_SETTLE) {
		return VOICE_FRAME;
	}
	if (avg > level_target + LEVEL_HYST) {
		return VOICE_FRAME + 1;
	}
	if (avg < level_target - LEVEL_HYST) {
		return VOICE_FRAME - 1;
	}
	return VOICE_FRAME;
}

/* Lay 'take' mau, co gian thanh 160 (noi suy tuyen tinh) */
static void pcm_pull(int16_t *dst, uint16_t take)
{
	if (take == VOICE_FRAME) {
		for (uint16_t j = 0; j < VOICE_FRAME; j++) {
			dst[j] = pcm_ring[(pr_rd + j) % PCM_RING];
		}
	} else {
		const uint32_t step = ((uint32_t)(take - 1) << 16) / (VOICE_FRAME - 1);
		uint32_t pos = 0;

		for (uint16_t j = 0; j < VOICE_FRAME; j++) {
			const uint32_t i = pos >> 16;
			const int32_t f = (int32_t)(pos & 0xFFFF);
			const int32_t a = pcm_ring[(pr_rd + i) % PCM_RING];
			const int32_t b = (i + 1 < take) ? pcm_ring[(pr_rd + i + 1) % PCM_RING] : a;

			dst[j] = (int16_t)(a + (((b - a) * f) >> 16));
			pos += step;
		}
	}
	pr_rd += take;
}

/* 16 -> 32 kHz: mau chan = mau goc, mau le = Hermite tai diem giua (tre 1 mau) */
static void upsample2(const int16_t *in, int16_t *out)
{
	for (uint16_t n = 0; n < VOICE_FRAME; n++) {
		float xm1;
		float x0;
		float x1;
		float x2;
		float y;

		up_h[0] = up_h[1];
		up_h[1] = up_h[2];
		up_h[2] = up_h[3];
		up_h[3] = (float)in[n];
		xm1 = up_h[0];
		x0 = up_h[1];
		x1 = up_h[2];
		x2 = up_h[3];

		/* Catmull-Rom tai t = 0.5: x0 + c1/2 + c2/4 + c3/8 */
		y = x0 + 0.5f * (0.5f * (x1 - xm1)) +
		    0.25f * (xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2) +
		    0.125f * (0.5f * (x2 - xm1) + 1.5f * (x0 - x1));
		out[2 * n] = (int16_t)x0;
		out[2 * n + 1] = (int16_t)CLAMP((int32_t)y, INT16_MIN, INT16_MAX);
	}
}

static void encode_and_queue(uint16_t take)
{
	static int16_t pcm[VOICE_FRAME];
	static int16_t pcm2[2 * VOICE_FRAME];
	uint8_t out[SDU_MAX];
	const int16_t *src = pcm;
	struct net_buf *buf;
	uint32_t t0;
	int err;

	if (take > VOICE_FRAME) {
		st.slip_drop++;
	} else if (take < VOICE_FRAME) {
		st.slip_insert++;
	}
	pcm_pull(pcm, take);

	if (tx_cfg.freq_hz == 32000) {
		upsample2(pcm, pcm2);
		src = pcm2;
	}

	t0 = k_cycle_get_32();
	err = lc3_encode(enc, LC3_PCM_FORMAT_S16, src, 1, tx_cfg.octets_per_frame, out);
	perf_add(&perf_enc, k_cycle_get_32() - t0);
	if (err) {
		LOG_WRN("LC3 encode failed (%d)", err);
		return;
	}

	buf = make_sdu(out, tx_cfg.octets_per_frame);
	if (buf != NULL) {
		q_push(buf);
	}
}

/* ---------------- Thread giong noi ---------------- */

static bool session_alive(atomic_val_t my)
{
	return atomic_get(&active) && atomic_get(&session) == my;
}

static void run_session(atomic_val_t my)
{
	static int16_t zeros_lr[2 * VOICE_FRAME];
	int16_t ref[VOICE_FRAME];
	int16_t out[VOICE_FRAME];
	struct vdsp_stats ds;
	uint32_t mic_t0 = 0;
	uint32_t mic_pos = 0;
	uint32_t ref_gen = UINT32_MAX;
	int64_t ref_off = 0;
	bool ref_ok = false;
	bool mic_ok;
	int mic_fail_run = 0;
	float tone_phase = 0.0f;

	enc = lc3_setup_encoder(10000, tx_cfg.freq_hz, 0, &enc_mem);
	if (enc == NULL) {
		LOG_ERR("LC3 encoder setup failed (%u Hz)", tx_cfg.freq_hz);
		return;
	}
	/* Khung im lang ma hoa san: dung khi hang SDU rong */
	if (tx_cfg.freq_hz == 32000) {
		static int16_t z32[2 * VOICE_FRAME];

		(void)lc3_encode(enc, LC3_PCM_FORMAT_S16, z32, 1, tx_cfg.octets_per_frame,
				 silence_sdu);
	} else {
		(void)lc3_encode(enc, LC3_PCM_FORMAT_S16, zeros_lr, 1, tx_cfg.octets_per_frame,
				 silence_sdu);
	}

	vdsp_reset();
	pr_rd = 0;
	pr_wr = 0;
	settle = 0;
	level_avg_x16 = 0;
	memset(up_h, 0, sizeof(up_h));
	q_flush();
	atomic_set(&tx_seq, 0);
	st.sessions++;
	st.out_rate = tx_cfg.freq_hz;

	/* Moi: controller luon co du lieu de gui */
	for (int i = 0; i < PRIME_SDUS; i++) {
		send_silence();
	}

	mic_ok = mic_in_start(&mic_t0) == 0;
	st.mic_ok = mic_ok;
	if (!mic_ok) {
		LOG_WRN("Microphone not available - sending silence");
		call_ctrl_mic_unavailable(true);
	}
	LOG_INF("Voice TX start: %u Hz, %u B/SDU, mic %u Hz%s", tx_cfg.freq_hz,
		tx_cfg.octets_per_frame, VOICE_RATE, tx_cfg.freq_hz == 32000 ? " x2" : "");

	while (session_alive(my)) {
		int16_t *blk = NULL;
		const int16_t *lr;
		uint32_t a_pos;
		uint32_t a_cyc;
		uint32_t a_gen;
		uint32_t t0;

		if (mic_ok) {
			const int err = mic_in_read(&blk, 30);

			if (err) {
				st.mic_err++;
				blk = NULL;
				if (st.mic_err < 5 || (st.mic_err % 100) == 0) {
					LOG_WRN("Mic read failed (%d), %u errors", err, st.mic_err);
				}
				if (++mic_fail_run >= MIC_RESTART_AFTER) {
					mic_fail_run = 0;
					if (mic_in_restart(&mic_t0) == 0) {
						st.mic_restart++;
						mic_pos = 0; /* khoi doc ke tiep la mau 0 cua lan chay moi */
						ref_gen = UINT32_MAX; /* can lai tham chieu AEC */
						LOG_WRN("Microphones restarted (%u)", st.mic_restart);
						continue;
					} else {
						mic_ok = false;
						st.mic_ok = false;
					}
				}
			} else {
				mic_fail_run = 0;
			}
		} else {
			k_msleep(10);
		}
		lr = blk ? blk : zeros_lr;
		st.mic_blocks++;

		/* Tham chieu: can lai khi dau ra loa khoi dong lai */
		if (audio_out_ref_anchor(&a_pos, &a_cyc, &a_gen) && mic_ok) {
			if (a_gen != ref_gen) {
				const int32_t dcyc = (int32_t)(mic_t0 - a_cyc);

				ref_off = (int64_t)a_pos +
					  (int64_t)dcyc * VOICE_RATE / sys_clock_hw_cycles_per_sec() -
					  AEC_MARGIN;
				ref_gen = a_gen;
				vdsp_aec_reset();
				LOG_INF("AEC reference aligned (gen %u, offset %lld)", a_gen,
					(long long)(ref_off - a_pos));
			}
			ref_ok = audio_out_ref_read(ref, VOICE_FRAME,
						    (uint32_t)(ref_off + mic_pos)) == VOICE_FRAME;
		} else {
			ref_ok = false;
		}
		if (!ref_ok) {
			st.ref_missing++;
		}

		t0 = k_cycle_get_32();
		vdsp_process(lr, ref_ok ? ref : NULL, out);
		perf_add(&perf_dsp, k_cycle_get_32() - t0);

		mic_in_free(blk);
		mic_pos += VOICE_FRAME;

		if (atomic_get(&muted)) {
			memset(out, 0, sizeof(out));
		} else if (atomic_get(&tone_hz) != 0) {
			/* Am thu -12 dBFS thay cho giong noi (dat sau DSP: giam on khong nuot mat) */
			const float inc = 2.0f * 3.14159265f * (float)atomic_get(&tone_hz) / VOICE_RATE;

			for (int n = 0; n < VOICE_FRAME; n++) {
				out[n] = (int16_t)(8192.0f * sinf(tone_phase));
				tone_phase += inc;
				if (tone_phase > 2.0f * 3.14159265f) {
					tone_phase -= 2.0f * 3.14159265f;
				}
			}
		}
		pcm_push(out, VOICE_FRAME);
		level_update();

		for (;;) {
			const uint16_t take = frame_take();

			if (pr_count() < take) {
				break;
			}
			encode_and_queue(take);
		}

		/* Chuoi `sent` bi dut (gui loi) -> moi lai */
		if (k_cyc_to_ms_floor32(k_cycle_get_32() - (uint32_t)atomic_get(&last_sent_cyc)) >
		    SENT_WATCHDOG_MS) {
			send_silence();
		}
	}

	if (mic_ok) {
		mic_in_stop();
	}
	q_flush();
	vdsp_get_stats(&ds);
	LOG_INF("Voice TX stop: sent %u SDU, underrun %u, mic err %u, restart %u, ERLE %d dB",
		st.sdu_sent, st.sdu_underrun, st.mic_err, st.mic_restart, (int)ds.erle_db);
}

static void voice_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	vdsp_init();

	while (1) {
		atomic_val_t my;

		k_sem_take(&start_sem, K_FOREVER);
		my = atomic_get(&session);
		if (!session_alive(my)) {
			continue;
		}
		run_session(my);
	}
}

K_THREAD_DEFINE(voice_tid, VOICE_STACK, voice_thread, NULL, NULL, NULL, VOICE_PRIO, K_FP_REGS,
		0);

/* ---------------- API ---------------- */

void voice_start(struct bt_bap_stream *stream, const struct helmet_stream_cfg *cfg)
{
	tx_stream = stream;
	tx_cfg = *cfg;
	atomic_set(&last_sent_cyc, (atomic_val_t)k_cycle_get_32());
	atomic_inc(&session);
	atomic_set(&active, 1);
	st.active = true;
	k_sem_give(&start_sem);
}

void voice_stop(void)
{
	atomic_set(&active, 0);
	atomic_inc(&session);
	st.active = false;
}

void voice_set_tone(uint16_t hz)
{
	atomic_set(&tone_hz, hz);
}

void voice_set_mute(bool mute)
{
	atomic_set(&muted, mute ? 1 : 0);
	st.muted = mute;
}

bool voice_is_muted(void)
{
	return atomic_get(&muted) != 0;
}

void voice_set_dsp(bool aec, bool ns, bool agc)
{
	vdsp_set(aec, ns, agc);
}

void voice_get_dsp(bool *aec, bool *ns, bool *agc)
{
	vdsp_get_flags(aec, ns, agc);
}

void voice_get_stats(struct helmet_voice_stats *out)
{
	struct vdsp_stats ds;

	vdsp_get_stats(&ds);
	*out = st;
	out->mic_dbfs_x10 = (int16_t)(ds.mic_dbfs * 10.0f);
	out->out_dbfs_x10 = (int16_t)(ds.out_dbfs * 10.0f);
	out->erle_db_x10 = (int16_t)(ds.erle_db * 10.0f);
}

static int x10_int(int16_t v)
{
	return v / 10;
}

static int x10_frac(int16_t v)
{
	return (v < 0 ? -v : v) % 10;
}

void voice_print_status(const struct shell *sh)
{
	struct helmet_voice_stats v;
	struct vdsp_stats ds;
	bool aec;
	bool ns;
	bool agc;

	voice_get_stats(&v);
	vdsp_get_stats(&ds);
	vdsp_get_flags(&aec, &ns, &agc);
	shell_print(sh, "Voice     : %s, %u Hz, mic %s, %s", v.active ? "ACTIVE" : "idle",
		    v.out_rate, v.mic_ok ? "ok" : "n/a", v.muted ? "MUTED" : "unmuted");
	shell_print(sh, "DSP       : AEC %s, NS %s, AGC %s", aec ? "on" : "off", ns ? "on" : "off",
		    agc ? "on" : "off");
	shell_print(sh, "Levels    : mic %d.%d dBFS, out %d.%d dBFS, ERLE %d.%d dB", x10_int(v.mic_dbfs_x10),
		    x10_frac(v.mic_dbfs_x10), x10_int(v.out_dbfs_x10), x10_frac(v.out_dbfs_x10),
		    x10_int(v.erle_db_x10), x10_frac(v.erle_db_x10));
	shell_print(sh, "AEC       : far %u, double-talk %u, resets %u, ref missing %u",
		    ds.far_blocks, ds.dt_blocks, ds.resets, v.ref_missing);
	shell_print(sh, "TX        : sessions %u, blocks %u, sent %u, underrun %u, send err %u, "
		    "pool empty %u, q drop %u, slip -%u/+%u, mic err %u, mic restart %u",
		    v.sessions, v.mic_blocks, v.sdu_sent, v.sdu_underrun, v.send_err,
		    v.pool_empty, v.q_drop, v.slip_drop, v.slip_insert, v.mic_err, v.mic_restart);
}

static void print_perf(const struct shell *sh, const char *name, const struct perf *p)
{
	const uint32_t avg_us = k_cyc_to_us_floor32(p->n ? p->sum / p->n : 0);

	shell_print(sh, "%-16s: avg %u us, max %u us (%u%% of 10 ms), n %u", name, avg_us,
		    k_cyc_to_us_floor32(p->max), avg_us / 100U, p->n);
}

void voice_print_perf(const struct shell *sh)
{
	print_perf(sh, "Voice DSP", &perf_dsp);
	print_perf(sh, "LC3 encode", &perf_enc);
}
