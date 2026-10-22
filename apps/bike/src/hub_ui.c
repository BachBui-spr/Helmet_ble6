/*
 * Bike hub - giao dien: 4 nut, am bao qua loa (I2S20 -> 2 x MAX98357A), bat / tat nguon.
 * Board hub giong het board mu (apps/main): cung chan nut, cung loa, cung cach bat / tat.
 *
 * Cu chi (thread "hub_ui", tu cac su kien gpio-keys):
 *   Power ngan (< 600 ms)              -> HUB_G_STATUS (huy cua so ghep neu dang mo)
 *   Power giu 3 s                      -> HUB_G_POWER_OFF
 *   Power ngan -> trong 2 s giu Vol+ 2 s -> HUB_G_PAIR (mo ghep mu 60 s)
 *   Vol+ va Vol- cung giu 5 s          -> HUB_G_UNPAIR
 *   Call ngan                          -> HUB_G_CALL (bat / tat che do Dinh vi)
 * Bat may: giu Power 2 s (thuc tu System OFF), nha som thi tat lai ngay.
 *
 * Am bao: thread "hub_tone" sinh song sin 16 kHz stereo, khoi 10 ms; am lap (dang tim) phat
 * toi khi co am khac hoac hub_tone_stop().
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>

#include "bike.h"

LOG_MODULE_REGISTER(hub_ui, LOG_LEVEL_INF);

/* ---------------- Am bao ---------------- */

#define TONE_RATE    16000
#define TONE_BLOCK   (TONE_RATE / 100) /* 10 ms */
#define TONE_BYTES   (TONE_BLOCK * 2 * sizeof(int16_t))
#define TONE_BLOCKS  6 /* 60 ms dem: thread am bao bi chiem CPU vai chuc ms van khong can */
#define TONE_AMP     4125.0f /* -18 dBFS */
#define TONE_RAMP    (TONE_RATE / 200) /* 5 ms doc len / xuong */
#define TONE_PI2     6.28318531f

struct seg {
	uint16_t freq; /* Hz, 0 = im lang */
	uint16_t ms;
	uint16_t gap_ms;
};

struct tone_def {
	const struct seg *seg;
	uint8_t n;
	bool loop;
};

static const struct seg t_power_on[] = {{880, 80, 20}, {1175, 80, 20}, {1568, 120, 0}};
static const struct seg t_power_off[] = {{1568, 80, 20}, {1175, 80, 20}, {880, 160, 0}};
static const struct seg t_connected[] = {{1319, 60, 60}, {1319, 60, 0}};
static const struct seg t_disconnected[] = {{659, 200, 0}};
static const struct seg t_pair_search[] = {{988, 50, 30}, {1319, 50, 30}, {1760, 70, 1200}};
static const struct seg t_helmet_joined[] = {{1319, 60, 20}, {1760, 150, 0}};
static const struct seg t_pair_end[] = {{1760, 60, 20}, {1319, 150, 0}};
static const struct seg t_fail[] = {{494, 120, 60}, {494, 120, 60}, {494, 120, 0}};
static const struct seg t_status_ok[] = {{1319, 80, 0}};
static const struct seg t_status_no_link[] = {{1319, 80, 80}, {1319, 80, 0}};
static const struct seg t_unpair[] = {{1568, 50, 50}, {1568, 50, 50}, {1568, 50, 50}, {1568, 50, 0}};
/* Dinh vi: 3 not di len / di xuong (giong am nav_on / nav_off cua mu) */
static const struct seg t_nav_on[] = {{659, 60, 20}, {988, 60, 20}, {1319, 100, 0}};
static const struct seg t_nav_off[] = {{1319, 60, 20}, {988, 60, 20}, {659, 100, 0}};

#define DEF(id, s, lp) [id] = {.seg = s, .n = ARRAY_SIZE(s), .loop = lp}

static const struct tone_def defs[HUB_TONE_COUNT] = {
	DEF(HUB_TONE_POWER_ON, t_power_on, false),
	DEF(HUB_TONE_POWER_OFF, t_power_off, false),
	DEF(HUB_TONE_CONNECTED, t_connected, false),
	DEF(HUB_TONE_DISCONNECTED, t_disconnected, false),
	DEF(HUB_TONE_PAIR_SEARCH, t_pair_search, true),
	DEF(HUB_TONE_HELMET_JOINED, t_helmet_joined, false),
	DEF(HUB_TONE_PAIR_END, t_pair_end, false),
	DEF(HUB_TONE_FAIL, t_fail, false),
	DEF(HUB_TONE_STATUS_OK, t_status_ok, false),
	DEF(HUB_TONE_STATUS_NO_LINK, t_status_no_link, false),
	DEF(HUB_TONE_UNPAIR, t_unpair, false),
	DEF(HUB_TONE_NAV_ON, t_nav_on, false),
	DEF(HUB_TONE_NAV_OFF, t_nav_off, false),
};

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s20));
K_MEM_SLAB_DEFINE_STATIC(tone_slab, TONE_BYTES, TONE_BLOCKS, 4);
K_MSGQ_DEFINE(tone_q, sizeof(uint8_t), 4, 1);
static atomic_t tone_gen; /* tang moi lan co yeu cau moi: am dang phat dung lai */
static atomic_t tone_busy;
static bool i2s_ok;

static int tone_i2s_setup(void)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = TONE_RATE,
		.mem_slab = &tone_slab,
		.block_size = TONE_BYTES,
		.timeout = 100,
	};

	if (!device_is_ready(i2s_dev)) {
		return -ENODEV;
	}
	return i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
}

/* Mot khoi 10 ms: song sin (co doc 5 ms o dau / cuoi doan) hoac im lang */
static int tone_block(float *phase, uint16_t freq, uint32_t pos, uint32_t len, bool *started,
		      uint8_t *queued)
{
	int16_t *blk;
	int err;

	if (k_mem_slab_alloc(&tone_slab, (void **)&blk, K_MSEC(100)) != 0) {
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < TONE_BLOCK; i++) {
		float v = 0.0f;

		if (freq != 0 && pos + i < len) {
			const uint32_t p = pos + i;
			float g = 1.0f;

			if (p < TONE_RAMP) {
				g = (float)p / TONE_RAMP;
			} else if (len - p < TONE_RAMP) {
				g = (float)(len - p) / TONE_RAMP;
			}
			v = sinf(*phase) * TONE_AMP * g;
			*phase += TONE_PI2 * freq / TONE_RATE;
			if (*phase > TONE_PI2) {
				*phase -= TONE_PI2;
			}
		}
		blk[2 * i] = (int16_t)v;
		blk[2 * i + 1] = (int16_t)v;
	}
	err = i2s_write(i2s_dev, blk, TONE_BYTES);
	if (err) {
		k_mem_slab_free(&tone_slab, blk);
		return err;
	}
	if (!*started && ++(*queued) >= 3) {
		err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		*started = (err == 0);
	}
	return 0;
}

static void tone_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint8_t id;
		atomic_val_t gen;
		bool started = false;
		uint8_t queued = 0;
		bool abort = false;

		(void)k_msgq_get(&tone_q, &id, K_FOREVER);
		if (!i2s_ok || id >= HUB_TONE_COUNT) {
			continue;
		}
		gen = atomic_get(&tone_gen);
		atomic_set(&tone_busy, 1);

		do {
			const struct tone_def *d = &defs[id];

			for (uint8_t s = 0; s < d->n && !abort; s++) {
				const uint32_t on = d->seg[s].ms * (TONE_RATE / 1000);
				const uint32_t off = d->seg[s].gap_ms * (TONE_RATE / 1000);
				float phase = 0.0f;

				for (uint32_t pos = 0; pos < on + off; pos += TONE_BLOCK) {
					if (atomic_get(&tone_gen) != gen ||
					    tone_block(&phase, pos < on ? d->seg[s].freq : 0, pos,
						       on, &started, &queued) != 0) {
						abort = true;
						break;
					}
				}
			}
		} while (defs[id].loop && !abort);

		if (started) {
			(void)i2s_trigger(i2s_dev, I2S_DIR_TX,
					  abort ? I2S_TRIGGER_DROP : I2S_TRIGGER_DRAIN);
		} else if (queued > 0) {
			(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		}
		/* Cho I2S tra het khoi (dung han) truoc am ke tiep: ghi luc dang dung se loi */
		for (int i = 0; i < 50 && k_mem_slab_num_used_get(&tone_slab) > 0; i++) {
			k_msleep(10);
		}
		atomic_set(&tone_busy, 0);
	}
}

K_THREAD_DEFINE(hub_tone_tid, 1536, tone_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(2), K_FP_REGS,
		0);

void hub_tone(enum hub_tone t)
{
	const uint8_t id = (uint8_t)t;

	atomic_inc(&tone_gen); /* dung am dang phat (vd am "dang tim" lap) */
	k_msgq_purge(&tone_q);
	(void)k_msgq_put(&tone_q, &id, K_NO_WAIT);
}

void hub_tone_stop(void)
{
	atomic_inc(&tone_gen);
	k_msgq_purge(&tone_q);
}

/* ---------------- Nguon ---------------- */

#define POWER_ON_HOLD_MS 2000
#define RELEASE_WAIT_MS  10000

static const struct gpio_dt_spec power_btn = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_power), gpios);
static atomic_t ignore_power; /* dang giu Power tu luc bat may: bo qua toi khi nha */

static FUNC_NORETURN void enter_system_off(bool wait_release)
{
	if (wait_release) {
		/* Thuc theo MUC "nhan": con dang giu thi chip thuc ngay */
		for (int i = 0; i < RELEASE_WAIT_MS / 20 && gpio_pin_get_dt(&power_btn) > 0; i++) {
			k_msleep(20);
		}
	}
	LOG_PANIC();
	k_busy_wait(100000);
	(void)gpio_pin_interrupt_configure_dt(&power_btn, GPIO_INT_LEVEL_ACTIVE);
	(void)hwinfo_clear_reset_cause();
	sys_poweroff();
}

bool hub_power_boot_check(void)
{
	uint32_t cause = 0;
	bool by_button = false;

	(void)hwinfo_get_reset_cause(&cause);
	if (cause & RESET_LOW_POWER_WAKE) {
		while (k_uptime_get() < POWER_ON_HOLD_MS) {
			if (gpio_pin_get_dt(&power_btn) <= 0) {
				enter_system_off(false); /* nha som: bat nham, tat lai */
			}
			k_msleep(20);
		}
		by_button = true;
	}
	(void)hwinfo_clear_reset_cause();
	LOG_INF("Boot: reset cause 0x%08x%s", cause, by_button ? " (Power held 2 s -> ON)" : "");
	if (by_button || gpio_pin_get_dt(&power_btn) > 0) {
		atomic_set(&ignore_power, 1);
	}
	return by_button;
}

FUNC_NORETURN void hub_power_off(void)
{
	LOG_INF("Power off (button) - hold Power 2 s to turn on");
	hub_tone(HUB_TONE_POWER_OFF);
	k_msleep(600);
	hub_bt_shutdown();
	enter_system_off(true);
}

/* ---------------- Nut ---------------- */

#define T_SHORT_MAX  600
#define T_POWER_OFF  3000
#define T_SEQ_WINDOW 2000
#define T_SEQ_HOLD   2000
#define T_COMBO      5000

enum { K_POWER, K_VOLUP, K_VOLDOWN, K_CALL, K_COUNT };

struct key_evt {
	uint8_t key;
	uint8_t pressed;
	int64_t t;
};

K_MSGQ_DEFINE(key_q, sizeof(struct key_evt), 16, 4);

static void on_input(struct input_event *evt, void *user_data)
{
	struct key_evt e = {.pressed = evt->value != 0, .t = k_uptime_get()};

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	switch (evt->code) {
	case INPUT_KEY_POWER:
		e.key = K_POWER;
		break;
	case INPUT_KEY_VOLUMEUP:
		e.key = K_VOLUP;
		break;
	case INPUT_KEY_VOLUMEDOWN:
		e.key = K_VOLDOWN;
		break;
	case INPUT_KEY_PLAYPAUSE:
		e.key = K_CALL;
		break;
	default:
		return;
	}
	(void)k_msgq_put(&key_q, &e, K_NO_WAIT);
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

static void ui_thread(void *p1, void *p2, void *p3)
{
	bool down[K_COUNT] = {0};
	int64_t t_down[K_COUNT] = {0};
	bool power_fired = false;
	int64_t seq_until = 0;   /* sau Power ngan: cua so cho giu Vol+ */
	bool seq_active = false; /* Vol+ dang giu trong cua so */
	bool seq_fired = false;
	int64_t combo_t = 0;
	bool combo_fired = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct key_evt e;
		int64_t now = k_uptime_get();
		int64_t deadline = INT64_MAX;
		k_timeout_t to;

		/* Hen gio gan nhat cua cac cu chi "giu" */
		if (down[K_POWER] && !power_fired && !atomic_get(&ignore_power)) {
			deadline = MIN(deadline, t_down[K_POWER] + T_POWER_OFF);
		}
		if (seq_active && !seq_fired) {
			deadline = MIN(deadline, t_down[K_VOLUP] + T_SEQ_HOLD);
		}
		if (combo_t && !combo_fired) {
			deadline = MIN(deadline, combo_t + T_COMBO);
		}
		to = deadline == INT64_MAX ? K_FOREVER : K_MSEC(MAX(deadline - now, 0));

		if (k_msgq_get(&key_q, &e, to) != 0) {
			/* Het gio: mot cu chi giu da du thoi gian */
			now = k_uptime_get();
			if (down[K_POWER] && !power_fired && !atomic_get(&ignore_power) &&
			    now - t_down[K_POWER] >= T_POWER_OFF) {
				power_fired = true;
				hub_gesture(HUB_G_POWER_OFF);
			}
			if (seq_active && !seq_fired && now - t_down[K_VOLUP] >= T_SEQ_HOLD) {
				seq_fired = true;
				hub_gesture(HUB_G_PAIR);
			}
			if (combo_t && !combo_fired && now - combo_t >= T_COMBO) {
				combo_fired = true;
				hub_gesture(HUB_G_UNPAIR);
			}
			continue;
		}

		if (e.pressed) {
			down[e.key] = true;
			t_down[e.key] = e.t;
			if (e.key == K_POWER) {
				power_fired = false;
			}
			if (e.key == K_VOLUP && e.t <= seq_until) {
				seq_active = true;
				seq_fired = false;
			}
			if ((e.key == K_VOLUP || e.key == K_VOLDOWN) && down[K_VOLUP] &&
			    down[K_VOLDOWN]) {
				combo_t = e.t;
				combo_fired = false;
				seq_active = false;
			}
			continue;
		}

		/* Nha phim */
		down[e.key] = false;
		switch (e.key) {
		case K_POWER:
			if (atomic_get(&ignore_power)) {
				atomic_set(&ignore_power, 0); /* nha sau lan giu de bat may */
			} else if (!power_fired && e.t - t_down[K_POWER] < T_SHORT_MAX) {
				seq_until = e.t + T_SEQ_WINDOW;
				hub_gesture(HUB_G_STATUS);
			}
			break;
		case K_VOLUP:
			seq_active = false;
			break;
		case K_CALL:
			if (e.t - t_down[K_CALL] < T_SHORT_MAX) {
				hub_gesture(HUB_G_CALL);
			}
			break;
		default:
			break;
		}
		if (!down[K_VOLUP] || !down[K_VOLDOWN]) {
			combo_t = 0;
		}
	}
}

K_THREAD_DEFINE(hub_ui_tid, 2048, ui_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);

int hub_ui_init(void)
{
	int err = tone_i2s_setup();

	i2s_ok = (err == 0);
	if (err) {
		LOG_WRN("I2S for prompts not available (%d) - no tones", err);
	}
	return 0;
}
