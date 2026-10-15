/*
 * Helmet - duong xuat am thanh: SDU LC3 -> giai ma (liblc3) -> PCM stereo -> I2S20 -> MAX98357A
 *
 * Luong du lieu:
 *   BT RX thread (stream_recv) --audio_q--> audio thread: giai ma, ghep kenh L/R, am luong
 *                                           --i2s_slab--> I2S20 (EasyDMA) -> 2 ampli
 *
 * - Moi khung LC3 (7.5/10 ms) tao ra dung 1 khoi PCM cho I2S. SDU bi mat van duoc
 *   controller bao len (co co LOST/ERROR) -> giai ma voi NULL de liblc3 noi suy (PLC),
 *   nho do I2S luon co du lieu deu dan.
 * - Clock cua dien thoai va cua Helmet lech nhau vai chuc ppm -> so khoi cho trong I2S
 *   troi dan. Khi bo dem qua day thi bo 1 khung, khi sap can thi lap lai 1 khung
 *   (khoang vai phut mot lan voi lech ~50 ppm).
 * - Moi thao tac giai ma (dung FPU) chi chay trong audio thread.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lc3.h>

#include "helmet.h"

LOG_MODULE_REGISTER(audio_out, LOG_LEVEL_INF);

#define MAX_FREQ_HZ       48000
#define MAX_FRAME_US      10000
#define MAX_FRAME_SAMPLES (MAX_FREQ_HZ * MAX_FRAME_US / USEC_PER_SEC) /* 480 */
#define I2S_BLOCK_MAX     (MAX_FRAME_SAMPLES * 2 * sizeof(int16_t))    /* 1920 B */

/* So khoi PCM: driver nRF giu toi da 2 khoi + hang doi TX 4 khoi */
#define I2S_BLOCK_COUNT 8
#define PREFILL_BLOCKS  3 /* bat dau phat khi da co 3 khung (~30 ms dem jitter) */
#define HIGH_WATER      5 /* >= : bo dem qua day -> bo 1 khung */
#define LOW_WATER       1 /* <= : bo dem sap can -> lap 1 khung */

#define CH_L BIT(0)
#define CH_R BIT(1)

#define AUDIO_THREAD_STACK 4096
#define AUDIO_THREAD_PRIO  K_PRIO_PREEMPT(2)

enum audio_msg_type {
	MSG_START,
	MSG_STOP,
	MSG_DATA,
};

struct audio_msg {
	uint8_t type;
	uint8_t idx;
	uint8_t valid;
	uint16_t len;
	uint32_t ts;
	union {
		struct helmet_stream_cfg cfg;
		uint8_t data[HELMET_MAX_SDU_LEN];
	};
};

K_MSGQ_DEFINE(audio_q, sizeof(struct audio_msg), 8, 4);
K_MEM_SLAB_DEFINE_STATIC(i2s_slab, I2S_BLOCK_MAX, I2S_BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s20));

/* Trang thai giai ma cua tung luong (ASE) */
struct dec_stream {
	bool active;
	struct helmet_stream_cfg cfg;
	uint8_t out_mask[2]; /* kenh LC3 thu c -> loa CH_L / CH_R / ca hai */
	lc3_decoder_t dec[2];
	lc3_decoder_mem_48k_t mem[2];
};

static struct dec_stream ds[HELMET_MAX_STREAMS];

/* Khung PCM stereo dang ghep (L, R xen ke) */
static int16_t frame_pcm[MAX_FRAME_SAMPLES * 2];
static uint8_t frame_mask;    /* kenh da co du lieu trong khung hien tai */
static uint8_t expected_mask; /* kenh cac luong dang hoat dong se cung cap */
static uint32_t frame_ts;

static uint32_t out_rate;
static uint16_t out_samples;
static bool i2s_configured;
static bool i2s_running;
static uint8_t prefill;

/* He so am luong Q15 (32767 = 0 dB) */
static atomic_t gain_q15 = ATOMIC_INIT(32767);

static struct helmet_audio_stats stats;

/* ---------------- I2S ---------------- */

static uint8_t blocks_in_use(void)
{
	return I2S_BLOCK_COUNT - k_mem_slab_num_free_get(&i2s_slab);
}

static void wait_blocks_released(void)
{
	for (int i = 0; i < 50 && blocks_in_use() > 0; i++) {
		k_msleep(2);
	}
}

/* Dung I2S ngay lap tuc va bo du lieu dang cho (dung khi dung phat / loi) */
static void i2s_halt(void)
{
	if (!i2s_configured) {
		return;
	}

	if (i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP) != 0) {
		/* Trang thai ERROR (underrun) can PREPARE de ve READY */
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	}
	wait_blocks_released();
	i2s_running = false;
	prefill = 0;
}

static int i2s_setup(uint32_t rate, uint16_t samples)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = rate,
		.mem_slab = &i2s_slab,
		.block_size = samples * 2 * sizeof(int16_t),
		.timeout = 20,
	};
	int err;

	i2s_halt();

	err = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (err) {
		LOG_ERR("i2s_configure %u Hz failed (%d)", rate, err);
		i2s_configured = false;
		return err;
	}

	i2s_configured = true;
	out_rate = rate;
	out_samples = samples;
	LOG_INF("I2S configured: %u Hz, %u samples/frame, BCLK P1.12 LRCLK P1.10 DIN P1.15",
		rate, samples);
	return 0;
}

static void i2s_submit(const int16_t *pcm)
{
	const size_t size = out_samples * 2 * sizeof(int16_t);
	uint8_t used = blocks_in_use();
	int copies = 1;

	if (!i2s_configured) {
		return;
	}

	if (i2s_running) {
		if (used >= HIGH_WATER) {
			stats.drift_drop++;
			return;
		}
		if (used <= LOW_WATER) {
			copies = 2;
			stats.drift_repeat++;
		}
	}

	for (int n = 0; n < copies; n++) {
		void *blk;
		int err;

		if (k_mem_slab_alloc(&i2s_slab, &blk, K_NO_WAIT) != 0) {
			stats.drift_drop++;
			return;
		}
		memcpy(blk, pcm, size);

		err = i2s_write(i2s_dev, blk, size);
		if (err) {
			k_mem_slab_free(&i2s_slab, blk);
			stats.i2s_err++;
			LOG_WRN("I2S write failed (%d) - restarting output", err);
			i2s_halt();
			return;
		}

		if (!i2s_running && ++prefill >= PREFILL_BLOCKS) {
			err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			if (err) {
				stats.i2s_err++;
				LOG_WRN("I2S start failed (%d)", err);
				i2s_halt();
				return;
			}
			i2s_running = true;
			LOG_INF("I2S output running (%u Hz)", out_rate);
		}
	}
}

/* ---------------- Ghep khung va am luong ---------------- */

static void flush_frame(void)
{
	const int32_t gain = (int32_t)atomic_get(&gain_q15);

	if (frame_mask == 0) {
		return;
	}

	/* Thieu mot ben -> chep ben con lai sang (mu co 2 loa, luon phat ca hai) */
	for (uint16_t i = 0; i < out_samples; i++) {
		int16_t *l = &frame_pcm[2 * i];
		int16_t *r = &frame_pcm[2 * i + 1];

		if (!(frame_mask & CH_L)) {
			*l = *r;
		} else if (!(frame_mask & CH_R)) {
			*r = *l;
		}
		*l = (int16_t)(((int32_t)*l * gain) >> 15);
		*r = (int16_t)(((int32_t)*r * gain) >> 15);
	}

	i2s_submit(frame_pcm);
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

static void handle_start(uint8_t idx, const struct helmet_stream_cfg *cfg)
{
	struct dec_stream *d = &ds[idx];
	int samples = lc3_frame_samples(cfg->frame_dur_us, cfg->freq_hz);

	if (samples <= 0 || samples > MAX_FRAME_SAMPLES || cfg->chan_cnt == 0 ||
	    cfg->chan_cnt > 2) {
		LOG_ERR("Stream %u: unsupported config (%u Hz, %u us, %u ch)", idx, cfg->freq_hz,
			cfg->frame_dur_us, cfg->chan_cnt);
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
	d->active = true;
	update_expected_mask();

	if (!i2s_configured || out_rate != cfg->freq_hz || out_samples != samples) {
		frame_mask = 0;
		(void)i2s_setup(cfg->freq_hz, (uint16_t)samples);
	}

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
		frame_mask = 0;
		i2s_halt();
		LOG_INF("I2S output stopped");
	}
}

static void handle_data(struct audio_msg *m)
{
	struct dec_stream *d = &ds[m->idx];
	const uint16_t octets = d->cfg.octets_per_frame;
	bool valid = m->valid;

	if (!d->active || !i2s_configured) {
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

	for (uint8_t c = 0; c < d->cfg.chan_cnt; c++) {
		const uint8_t mask = d->out_mask[c];
		int16_t *dst = (mask & CH_L) ? &frame_pcm[0] : &frame_pcm[1];
		const void *in = valid ? &m->data[c * octets] : NULL; /* NULL -> PLC */
		int ret = lc3_decode(d->dec[c], in, octets, LC3_PCM_FORMAT_S16, dst, 2);

		if (ret == 1) {
			stats.plc++;
		} else if (ret < 0) {
			stats.dec_err++;
			for (uint16_t i = 0; i < out_samples; i++) {
				dst[2 * i] = 0;
			}
		}

		if (mask == (CH_L | CH_R)) {
			for (uint16_t i = 0; i < out_samples; i++) {
				frame_pcm[2 * i + 1] = frame_pcm[2 * i];
			}
		}
		frame_mask |= mask;
	}

	if ((frame_mask & expected_mask) == expected_mask) {
		flush_frame();
	}
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	static struct audio_msg msg;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msgq_get(&audio_q, &msg, K_FOREVER);

		switch (msg.type) {
		case MSG_START:
			handle_start(msg.idx, &msg.cfg);
			break;
		case MSG_STOP:
			handle_stop(msg.idx);
			break;
		case MSG_DATA:
			handle_data(&msg);
			break;
		default:
			break;
		}
	}
}

K_THREAD_DEFINE(audio_tid, AUDIO_THREAD_STACK, audio_thread, NULL, NULL, NULL, AUDIO_THREAD_PRIO,
		K_FP_REGS, 0);

/* ---------------- API cho cac module khac ---------------- */

int audio_out_init(void)
{
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S20 not ready");
		return -ENODEV;
	}
	LOG_INF("Audio out ready: LC3 decoder (liblc3) -> I2S20 -> 2 x MAX98357A");
	return 0;
}

void audio_out_stream_start(uint8_t idx, const struct helmet_stream_cfg *cfg)
{
	struct audio_msg msg = {.type = MSG_START, .idx = idx, .cfg = *cfg};

	if (idx >= ARRAY_SIZE(ds) || k_msgq_put(&audio_q, &msg, K_MSEC(100)) != 0) {
		LOG_ERR("Stream %u: start message lost", idx);
	}
}

void audio_out_stream_stop(uint8_t idx)
{
	struct audio_msg msg = {.type = MSG_STOP, .idx = idx};

	if (idx >= ARRAY_SIZE(ds) || k_msgq_put(&audio_q, &msg, K_MSEC(100)) != 0) {
		LOG_ERR("Stream %u: stop message lost", idx);
	}
}

/* Goi tu BT RX thread: chi copy du lieu, khong giai ma o day */
void audio_out_stream_data(uint8_t idx, bool valid, uint32_t ts, const uint8_t *data,
			   uint16_t len)
{
	static struct audio_msg msg;

	if (idx >= ARRAY_SIZE(ds)) {
		return;
	}
	if (len > HELMET_MAX_SDU_LEN) {
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
	uint32_t g = mute ? 0 : ((uint32_t)volume * volume * 32767U) / (255U * 255U);

	atomic_set(&gain_q15, (atomic_val_t)g);
}

void audio_out_get_stats(struct helmet_audio_stats *out)
{
	*out = stats;
	out->out_rate = out_rate;
	out->frame_samples = out_samples;
	out->active_mask = expected_mask;
	out->i2s_running = i2s_running;
	out->queued_blocks = blocks_in_use();
}
