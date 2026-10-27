/*
 * Lop DICH VU: nhan dang cu chi nut cua mu (ngan / dup / giu / lap / to hop).
 *
 * Chay trong thread cua lop tren (bo dieu khien ung dung): lop tren dua su kien phim tho
 * (hal/keys.c) va su kien hen gio vao buttons_on_key() / buttons_on_timer() trong thread do,
 * nen may trang thai khong can khoa.
 */

#ifndef ME54_SERVICES_BUTTONS_H_
#define ME54_SERVICES_BUTTONS_H_

#include <stdbool.h>
#include <stdint.h>

#include <me54/hal/keys.h>

enum gesture_id {
	G_SHORT,
	G_DOUBLE,
	G_LONG,
	G_VERY_LONG,
	G_REPEAT,
	G_COMBO_VOL,
	G_SEQ_HOLD, /* Vol+/Vol-/Call giu 2 s ngay sau Power nhan ngan */
	G_COUNT,
};

/* Chi so hen gio to hop Vol+ + Vol- (cac phim dung chi so 0..KEY_COUNT-1) */
#define BUTTONS_COMBO_TIMER KEY_COUNT

struct buttons_cb {
	/* ISR hen gio: dua (idx, gen) ve thread lop tren, roi goi buttons_on_timer() o do */
	void (*post_timer)(uint8_t idx, uint32_t gen);
	/* Cu chi nhan duoc, trong thread lop tren */
	void (*gesture)(uint8_t key, uint8_t gesture);
};

void buttons_init(const struct buttons_cb *cb);
void buttons_on_key(uint8_t key, bool pressed, uint32_t t_ms);
void buttons_on_timer(uint8_t idx, uint32_t gen);
void buttons_set_double(enum key_id key, bool enabled);
void buttons_ignore_until_release(enum key_id key);
/* Sau Power nhan ngan: lan nhan Vol/Call dau tien trong window_ms la buoc 2 cua to hop */
void buttons_arm_seq(uint32_t window_ms);
const char *buttons_key_str(uint8_t key);
const char *buttons_gesture_str(uint8_t gesture);
int buttons_key_from_str(const char *s);
int buttons_gesture_from_str(const char *s);

#endif /* ME54_SERVICES_BUTTONS_H_ */
