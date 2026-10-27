/*
 * Bike hub - lop UNG DUNG: 4 nut, am bao qua loa, bat / tat nguon.
 * Board hub giong het board mu (apps/main): cung chan nut, cung loa, cung cach bat / tat, nen
 * dung chung lib/: hal/keys.c (nut), hal/power_hw.c (System OFF), services/prompt_player.c +
 * core/prompts.c (am bao). Cu chi cua hub khac mu nen nhan dang rieng o day.
 *
 * Cu chi (thread "hub_ui", tu cac su kien gpio-keys):
 *   Power ngan (< 600 ms)              -> HUB_G_STATUS (huy cua so ghep neu dang mo)
 *   Power giu 3 s                      -> HUB_G_POWER_OFF
 *   Power ngan -> trong 2 s giu Vol+ 2 s -> HUB_G_PAIR (mo ghep mu 60 s)
 *   Vol+ va Vol- cung giu 5 s          -> HUB_G_UNPAIR
 *   Call ngan                          -> HUB_G_CALL (de danh)
 * Bat may: giu Power 2 s (thuc tu System OFF), nha som thi tat lai ngay.
 *
 * Am bao: services/prompt_player.c (16 kHz stereo, khoi 10 ms); am lap (dang tim) phat toi khi
 * co am khac hoac hub_tone_stop(). Moi am cua hub trung voi mot am cua mu (core/prompts.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <me54/core/prompts.h>
#include <me54/hal/keys.h>
#include <me54/hal/power_hw.h>
#include <me54/services/prompt_player.h>

#include "app/app.h"

LOG_MODULE_REGISTER(hub_ui, LOG_LEVEL_INF);

/* ---------------- Am bao ---------------- */

/* Am cua hub -> am cung giai dieu trong danh muc chung (core/prompts.c) */
static const uint8_t tone_prompt[HUB_TONE_COUNT] = {
	[HUB_TONE_POWER_ON] = PROMPT_POWER_ON,
	[HUB_TONE_POWER_OFF] = PROMPT_POWER_OFF,
	[HUB_TONE_CONNECTED] = PROMPT_CONNECTED,
	[HUB_TONE_DISCONNECTED] = PROMPT_DISCONNECTED,
	[HUB_TONE_PAIR_SEARCH] = PROMPT_BC_SEARCH,
	[HUB_TONE_HELMET_JOINED] = PROMPT_BC_JOINED,
	[HUB_TONE_PAIR_END] = PROMPT_BC_LEFT,
	[HUB_TONE_FAIL] = PROMPT_BC_FAIL,
	[HUB_TONE_STATUS_OK] = PROMPT_STATUS_OK,
	[HUB_TONE_STATUS_NO_LINK] = PROMPT_STATUS_NO_LINK,
	[HUB_TONE_UNPAIR] = PROMPT_PAIRING,
	[HUB_TONE_NAV_ON] = PROMPT_NAV_ON,
	[HUB_TONE_NAV_OFF] = PROMPT_NAV_OFF,
};

void hub_tone(enum hub_tone t)
{
	if ((unsigned int)t >= HUB_TONE_COUNT) {
		prompt_player_stop();
		return;
	}
	prompt_player_play((enum prompt_id)tone_prompt[t]); /* dung am dang phat (vd "dang tim") */
}

void hub_tone_stop(void)
{
	prompt_player_stop();
}

/* ---------------- Nguon ---------------- */

static atomic_t ignore_power; /* dang giu Power tu luc bat may: bo qua toi khi nha */

static void on_key(uint8_t key, bool pressed, uint32_t t_ms);

bool hub_power_boot_check(void)
{
	uint32_t cause;
	bool by_button;
	bool held;

	/* Truoc tien: su kien nut tu luc khoi dong vao hang cua thread hub_ui */
	keys_init(on_key);
	held = power_hw_boot_check(&cause, &by_button); /* nha som: tat lai, khong tra ve */
	LOG_INF("Boot: reset cause 0x%08x%s", cause, by_button ? " (Power held 2 s -> ON)" : "");
	if (held) {
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
	power_hw_system_off(true);
}

/* ---------------- Nut ---------------- */

#define T_SHORT_MAX  600
#define T_POWER_OFF  3000
#define T_SEQ_WINDOW 2000
#define T_SEQ_HOLD   2000
#define T_COMBO      5000

struct key_evt {
	uint8_t key;
	uint8_t pressed;
	int64_t t;
};

K_MSGQ_DEFINE(key_q, sizeof(struct key_evt), 16, 4);

/* hal/keys.c (thread input) -> hang cua thread hub_ui */
static void on_key(uint8_t key, bool pressed, uint32_t t_ms)
{
	struct key_evt e = {.key = key, .pressed = pressed, .t = k_uptime_get()};

	ARG_UNUSED(t_ms);
	(void)k_msgq_put(&key_q, &e, K_NO_WAIT);
}

static void ui_thread(void *p1, void *p2, void *p3)
{
	bool down[KEY_COUNT] = {0};
	int64_t t_down[KEY_COUNT] = {0};
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
		if (down[KEY_POWER] && !power_fired && !atomic_get(&ignore_power)) {
			deadline = MIN(deadline, t_down[KEY_POWER] + T_POWER_OFF);
		}
		if (seq_active && !seq_fired) {
			deadline = MIN(deadline, t_down[KEY_VOL_UP] + T_SEQ_HOLD);
		}
		if (combo_t && !combo_fired) {
			deadline = MIN(deadline, combo_t + T_COMBO);
		}
		to = deadline == INT64_MAX ? K_FOREVER : K_MSEC(MAX(deadline - now, 0));

		if (k_msgq_get(&key_q, &e, to) != 0) {
			/* Het gio: mot cu chi giu da du thoi gian */
			now = k_uptime_get();
			if (down[KEY_POWER] && !power_fired && !atomic_get(&ignore_power) &&
			    now - t_down[KEY_POWER] >= T_POWER_OFF) {
				power_fired = true;
				hub_gesture(HUB_G_POWER_OFF);
			}
			if (seq_active && !seq_fired && now - t_down[KEY_VOL_UP] >= T_SEQ_HOLD) {
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
			if (e.key == KEY_POWER) {
				power_fired = false;
			}
			if (e.key == KEY_VOL_UP && e.t <= seq_until) {
				seq_active = true;
				seq_fired = false;
			}
			if ((e.key == KEY_VOL_UP || e.key == KEY_VOL_DOWN) && down[KEY_VOL_UP] &&
			    down[KEY_VOL_DOWN]) {
				combo_t = e.t;
				combo_fired = false;
				seq_active = false;
			}
			continue;
		}

		/* Nha phim */
		down[e.key] = false;
		switch (e.key) {
		case KEY_POWER:
			if (atomic_get(&ignore_power)) {
				atomic_set(&ignore_power, 0); /* nha sau lan giu de bat may */
			} else if (!power_fired && e.t - t_down[KEY_POWER] < T_SHORT_MAX) {
				seq_until = e.t + T_SEQ_WINDOW;
				hub_gesture(HUB_G_STATUS);
			}
			break;
		case KEY_VOL_UP:
			seq_active = false;
			break;
		case KEY_CALL:
			if (e.t - t_down[KEY_CALL] < T_SHORT_MAX) {
				hub_gesture(HUB_G_CALL);
			}
			break;
		default:
			break;
		}
		if (!down[KEY_VOL_UP] || !down[KEY_VOL_DOWN]) {
			combo_t = 0;
		}
	}
}

K_THREAD_DEFINE(hub_ui_tid, 2048, ui_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);

int hub_ui_init(void)
{
	const int err = prompt_player_init();

	if (err) {
		LOG_WRN("I2S for prompts not available (%d) - no tones", err);
	}
	return 0;
}
