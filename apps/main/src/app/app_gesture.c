/*
 * Helmet (apps/main) - lop UNG DUNG: cu chi nut -> hanh dong (chay trong thread bo dieu khien).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

#include <me54/hal/keys.h>
#include <me54/services/audio_out.h>
#include <me54/services/buttons.h>
#include <me54/services/voice_tx.h>

#include "app/app_internal.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/phone/call_ctrl.h"
#include "features/phone/le_audio.h"
#include "features/phone/media_ctrl.h"
#include "features/thermo/thermo.h"

LOG_MODULE_DECLARE(helmet, LOG_LEVEL_INF);

/* To hop intercom: sau Power nhan ngan co 2 s de bat dau giu Vol+ (master) / Vol- (slave) */
#define IC_SEQ_WINDOW_MS 2000

/* ---------------- Hanh dong theo cu chi (docs/HELMET_THIET_KE_CHUC_NANG_MOI.md muc 5.6) */

static void factory_unpair(void)
{
	int err;

	LOG_WRN("Vol+ + Vol- held 5 s: delete all bonds");
	if (app_bt_phone() != NULL) {
		(void)bt_conn_disconnect(app_bt_phone(), BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (err) {
		LOG_ERR("Unpair failed (%d)", err);
		return;
	}
	audio_out_prompt(PROMPT_PAIRING);
}

void app_gesture(uint8_t key, uint8_t g)
{
	int err = 0;

	if (app_state() == HS_SHUTDOWN || app_state() == HS_BOOT) {
		return;
	}
	power_activity();

	switch (key) {
	case KEY_CALL:
		if (g == G_SEQ_HOLD) {
			bcast_toggle(); /* Power ngan -> giu Call 2 s: tu tim / roi Auracast */
			break;
		}
		/* Cuoc goi dien thoai uu tien; sau do intercom (Call ngan: huy / ket thuc) */
		if (app_state() != HS_RINGING && app_state() != HS_IN_CALL && intercom_mode() != ICM_NORMAL) {
			if (g == G_SHORT) {
				intercom_stop();
			}
			break;
		}
		if (app_state() == HS_RINGING) {
			if (g == G_SHORT) {
				err = call_ctrl_accept();
			} else if (g == G_LONG) {
				err = call_ctrl_reject();
			}
		} else if (app_state() == HS_IN_CALL) {
			if (g == G_SHORT) {
				err = call_ctrl_hangup();
			} else if (g == G_DOUBLE) {
				err = call_ctrl_mic_mute(!voice_is_muted());
			}
		} else if (app_state() == HS_CONNECTED) {
			if (g == G_SHORT) {
				err = media_ctrl_play_pause();
			} else if (g == G_DOUBLE) {
				err = media_ctrl_next_track();
			}
		} else if (app_state() == HS_IDLE && g == G_SHORT) {
			audio_out_prompt(PROMPT_STATUS_NO_LINK);
		}
		break;

	case KEY_VOL_UP:
		if (g == G_SEQ_HOLD) {
			intercom_select(IC_ROLE_MASTER); /* Power ngan -> giu Vol+ 2 s */
		} else if (g == G_SHORT || g == G_REPEAT) {
			err = media_ctrl_volume_up();
		} else if (g == G_COMBO_VOL && (app_state() == HS_IDLE || app_state() == HS_CONNECTED)) {
			factory_unpair();
		}
		break;

	case KEY_VOL_DOWN:
		if (g == G_SEQ_HOLD) {
			intercom_select(IC_ROLE_SLAVE); /* Power ngan -> giu Vol- 2 s */
		} else if (g == G_SHORT || g == G_REPEAT) {
			err = media_ctrl_volume_down();
		}
		break;

	case KEY_POWER:
		if (g == G_VERY_LONG) {
			power_off(OFF_BUTTON);
		} else if (g == G_SHORT) {
			const enum ic_mode m = intercom_mode();

			if (m == ICM_SEARCH || m == ICM_WAIT || m == ICM_LINKING) {
				intercom_stop(); /* dang tim / cho: Power ngan = huy */
				break;
			}
			audio_out_prompt((app_bt_phone() || m == ICM_LINKED) ? PROMPT_STATUS_OK
									 : PROMPT_STATUS_NO_LINK);
			/* Buoc 1 cua to hop chon vai tro intercom */
			if (m == ICM_NORMAL && (app_state() == HS_IDLE || app_state() == HS_CONNECTED)) {
				buttons_arm_seq(IC_SEQ_WINDOW_MS);
			}
		}
		break;

	default:
		break;
	}

	if (err) {
		LOG_WRN("Action %s %s failed (%d)", buttons_key_str(key), buttons_gesture_str(g),
			err);
	}
}

