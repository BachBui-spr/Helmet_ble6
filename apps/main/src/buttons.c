/*
 * Helmet - nhan dang cu chi nut.
 *
 * gpio-keys (chong doi 30 ms) -> on_input (main.c) -> EVT_KEY_RAW
 * k_timer cua tung phim het gio (ISR)              -> EVT_KEY_TIMER
 * Bo dieu khien (thread main) goi buttons_handle() -> may trang thai tung phim
 *   -> app_gesture(phim, cu chi) ngay trong thread do (khong can khoa).
 *
 * Cu chi:
 *   G_SHORT     nha truoc 600 ms (neu phim co "dup" trong trang thai hien tai thi cho them
 *               350 ms xem co lan nhan thu hai khong)
 *   G_DOUBLE    lan nhan thu hai bat dau trong 350 ms sau lan nha dau
 *   G_LONG      giu toi 1 s (phat ngay luc do, khong cho nha)
 *   G_VERY_LONG giu toi 3 s
 *   G_REPEAT    Vol+/Vol- giu: lan dau sau 500 ms, sau do moi 250 ms
 *   G_COMBO_VOL Vol+ va Vol- cung nhan (cach nhau <= 200 ms) va giu 5 s
 * Nha trong khoang 600-1000 ms: khong phat gi (tranh nham ngan/giu).
 *
 * Moi lan hen gio tang "the he" (gen) cua phim; su kien hen gio mang gen cu (da bi huy
 * hoac hen lai) bi bo qua.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "helmet.h"

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

#define T_SHORT_MAX    600
#define T_DOUBLE_GAP   350
#define T_LONG         1000
#define T_VERY_LONG    3000
#define T_REPEAT_START 500
#define T_REPEAT       250
#define T_COMBO        5000
#define T_COMBO_PAIR   200

#define COMBO_TIMER KEY_COUNT

enum key_st {
	ST_IDLE,
	ST_DOWN1,  /* dang nhan lan 1 */
	ST_WAIT2,  /* da nha, cho lan nhan thu hai */
	ST_DOWN2,  /* dang nhan lan 2 */
	ST_HELD,   /* da phat G_LONG, dang giu */
	ST_HELD2,  /* da phat G_VERY_LONG, cho nha */
	ST_REPEAT, /* Vol: dang lap */
	ST_COMBO,  /* Vol+ va Vol- cung nhan */
	ST_IGNORE, /* bo qua toi lan nha ke tiep */
};

struct key_sm {
	uint8_t st;
	bool dbl;
	uint32_t t_down;
	uint32_t gen;
	struct k_timer timer;
};

static struct key_sm keys[KEY_COUNT];
static struct k_timer combo_timer;
static uint32_t combo_gen;
static bool combo_fired;

static const char *const key_names[KEY_COUNT] = {"volup", "voldown", "call", "power"};
static const char *const gesture_names[G_COUNT] = {"short", "double", "long", "vlong", "repeat",
						   "combo"};

static bool is_vol(uint8_t k)
{
	return k == KEY_VOL_UP || k == KEY_VOL_DOWN;
}

static void timer_expiry(struct k_timer *t)
{
	const uint32_t idx = (uint32_t)(uintptr_t)k_timer_user_data_get(t);
	const uint32_t gen = (idx == COMBO_TIMER) ? combo_gen : keys[idx].gen;

	app_post(EVT_KEY_TIMER, (uint8_t)idx, 0, gen);
}

static void start_timer(uint8_t k, uint32_t ms)
{
	keys[k].gen++;
	k_timer_start(&keys[k].timer, K_MSEC(ms), K_NO_WAIT);
}

static void stop_timer(uint8_t k)
{
	keys[k].gen++;
	k_timer_stop(&keys[k].timer);
}

static void emit(uint8_t k, uint8_t g)
{
	LOG_INF("Gesture %s %s", key_names[k], gesture_names[g]);
	app_gesture(k, g);
}

static void on_press(uint8_t k, uint32_t now)
{
	struct key_sm *s = &keys[k];

	switch (s->st) {
	case ST_IGNORE:
		return;
	case ST_WAIT2:
		stop_timer(k);
		s->st = ST_DOWN2;
		return;
	case ST_IDLE:
		break;
	default:
		return; /* su kien nhan lap (khong mong doi) */
	}

	if (is_vol(k)) {
		const uint8_t other = (k == KEY_VOL_UP) ? KEY_VOL_DOWN : KEY_VOL_UP;
		struct key_sm *o = &keys[other];

		if (o->st == ST_DOWN1 && (now - o->t_down) <= T_COMBO_PAIR) {
			stop_timer(other);
			o->st = ST_COMBO;
			s->st = ST_COMBO;
			combo_fired = false;
			combo_gen++;
			k_timer_start(&combo_timer, K_MSEC(T_COMBO), K_NO_WAIT);
			return;
		}
	}

	s->st = ST_DOWN1;
	s->t_down = now;
	start_timer(k, is_vol(k) ? T_REPEAT_START : T_LONG);
}

static void on_release(uint8_t k, uint32_t now)
{
	struct key_sm *s = &keys[k];

	switch (s->st) {
	case ST_DOWN1:
		stop_timer(k);
		s->st = ST_IDLE;
		if ((now - s->t_down) >= T_SHORT_MAX) {
			return; /* vung mo ho 600-1000 ms */
		}
		if (!is_vol(k) && s->dbl) {
			s->st = ST_WAIT2;
			start_timer(k, T_DOUBLE_GAP);
			return;
		}
		emit(k, G_SHORT);
		return;
	case ST_DOWN2:
		s->st = ST_IDLE;
		emit(k, G_DOUBLE);
		return;
	case ST_COMBO: {
		const uint8_t other = (k == KEY_VOL_UP) ? KEY_VOL_DOWN : KEY_VOL_UP;

		if (!combo_fired) {
			combo_gen++;
			k_timer_stop(&combo_timer);
		}
		s->st = ST_IDLE;
		if (keys[other].st == ST_COMBO) {
			keys[other].st = ST_IGNORE;
		}
		return;
	}
	case ST_IGNORE:
	case ST_HELD:
	case ST_HELD2:
	case ST_REPEAT:
		stop_timer(k);
		s->st = ST_IDLE;
		return;
	default:
		return;
	}
}

static void on_timer(uint8_t k, uint32_t gen)
{
	struct key_sm *s;

	if (k == COMBO_TIMER) {
		if (gen == combo_gen && keys[KEY_VOL_UP].st == ST_COMBO &&
		    keys[KEY_VOL_DOWN].st == ST_COMBO && !combo_fired) {
			combo_fired = true;
			emit(KEY_VOL_UP, G_COMBO_VOL);
		}
		return;
	}

	s = &keys[k];
	if (gen != s->gen) {
		return; /* hen gio da bi huy / hen lai */
	}

	switch (s->st) {
	case ST_DOWN1:
		if (is_vol(k)) {
			s->st = ST_REPEAT;
			start_timer(k, T_REPEAT);
			emit(k, G_REPEAT);
		} else {
			s->st = ST_HELD;
			start_timer(k, T_VERY_LONG - T_LONG);
			emit(k, G_LONG);
		}
		break;
	case ST_REPEAT:
		start_timer(k, T_REPEAT);
		emit(k, G_REPEAT);
		break;
	case ST_HELD:
		s->st = ST_HELD2;
		emit(k, G_VERY_LONG);
		break;
	case ST_WAIT2:
		s->st = ST_IDLE;
		emit(k, G_SHORT);
		break;
	default:
		break;
	}
}

void buttons_init(void)
{
	for (uint8_t k = 0; k < KEY_COUNT; k++) {
		keys[k].st = ST_IDLE;
		k_timer_init(&keys[k].timer, timer_expiry, NULL);
		k_timer_user_data_set(&keys[k].timer, (void *)(uintptr_t)k);
	}
	k_timer_init(&combo_timer, timer_expiry, NULL);
	k_timer_user_data_set(&combo_timer, (void *)(uintptr_t)COMBO_TIMER);
}

void buttons_handle(const struct helmet_evt *evt)
{
	if (evt->type == EVT_KEY_RAW && evt->a < KEY_COUNT) {
		LOG_DBG("Key %s %s", key_names[evt->a], evt->b ? "down" : "up");
		if (evt->b) {
			on_press(evt->a, evt->v);
		} else {
			on_release(evt->a, evt->v);
		}
	} else if (evt->type == EVT_KEY_TIMER && evt->a <= COMBO_TIMER) {
		on_timer(evt->a, evt->v);
	}
}

void buttons_set_double(enum helmet_key key, bool enabled)
{
	if (key < KEY_COUNT) {
		keys[key].dbl = enabled;
	}
}

void buttons_ignore_until_release(enum helmet_key key)
{
	if (key < KEY_COUNT) {
		stop_timer(key);
		keys[key].st = ST_IGNORE;
	}
}

const char *buttons_key_str(uint8_t key)
{
	return key < KEY_COUNT ? key_names[key] : "?";
}

const char *buttons_gesture_str(uint8_t gesture)
{
	return gesture < G_COUNT ? gesture_names[gesture] : "?";
}

int buttons_key_from_str(const char *s)
{
	for (int i = 0; i < KEY_COUNT; i++) {
		if (strcmp(key_names[i], s) == 0) {
			return i;
		}
	}
	return -1;
}

int buttons_gesture_from_str(const char *s)
{
	for (int i = 0; i < G_COUNT; i++) {
		if (strcmp(gesture_names[i], s) == 0) {
			return i;
		}
	}
	return -1;
}
