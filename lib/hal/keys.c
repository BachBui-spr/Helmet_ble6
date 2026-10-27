/*
 * Lop HAL - nut (xem keys.h).
 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>

#include <me54/hal/keys.h>

static keys_cb_t key_cb;

void keys_init(keys_cb_t cb)
{
	key_cb = cb;
}

static void on_input(struct input_event *evt, void *user_data)
{
	const keys_cb_t cb = key_cb;
	int key;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || cb == NULL) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_VOLUMEUP:
		key = KEY_VOL_UP;
		break;
	case INPUT_KEY_VOLUMEDOWN:
		key = KEY_VOL_DOWN;
		break;
	case INPUT_KEY_PLAYPAUSE:
		key = KEY_CALL;
		break;
	case INPUT_KEY_POWER:
		key = KEY_POWER;
		break;
	default:
		return;
	}
	cb((uint8_t)key, evt->value != 0, k_uptime_get_32());
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);
