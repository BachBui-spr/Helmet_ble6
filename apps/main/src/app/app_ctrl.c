/*
 * Helmet (apps/main) - lop UNG DUNG: bo dieu khien (chuong trinh chinh).
 * Thiet ke: docs/HELMET_THIET_KE_CHUC_NANG_MOI.md. Cau truc ma nguon: README.md muc "Kien truc ma nguon"
 *
 * Moi su kien (nut, ket noi, cuoc goi, luong am thanh, hen gio, canh bao nhiet, pin) tu cac
 * thread khac duoc dang vao app_q; thread main xu ly tuan tu -> khong can khoa, mot may trang
 * thai thiet bi duy nhat:
 *
 *   BOOT -> IDLE (quang ba) <-> CONNECTED <-> RINGING -> IN_CALL -> CONNECTED
 *                 \___________ Power giu 3 s / tu tat ___________/-> SHUTDOWN -> System OFF
 *
 * Vai tro Bluetooth: CAP Acceptor + BAP Unicast Server (2 sink + 1 source), TMAP UMR + CT,
 * VCP Volume Renderer, MCP client, CCP client (GTBS), MICP Microphone Device, HTS.
 * Log + shell qua RTT (make rtt) va UART20: lenh "helmet ..." (app_shell.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <me54/hal/keys.h>
#include <me54/hal/mic_in.h>
#include <me54/hal/status_led.h>
#include <me54/services/audio_out.h>
#include <me54/services/buttons.h>
#include <me54/services/voice_tx.h>

#include "app/app_internal.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/nav/nav.h"
#include "features/phone/call_ctrl.h"
#include "features/phone/le_audio.h"
#include "features/phone/media_ctrl.h"
#include "features/thermo/thermo.h"
#include "debug/capture.h"
#include "debug/gatt_trace.h"

LOG_MODULE_REGISTER(helmet, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS 10000
#define APP_Q_LEN           24
#define RING_CHECK_MS       1000

K_MSGQ_DEFINE(app_q, sizeof(struct helmet_evt), APP_Q_LEN, 4);

static enum helmet_state state = HS_BOOT;
static atomic_t q_overflow;
static bool temp_prompt_pending;

static void ring_check_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	app_post(EVT_RING_CHECK, 0, 0, 0);
}
static K_TIMER_DEFINE(ring_timer, ring_check_expiry, NULL);

/* hal/keys.c (thread input) -> hang su kien */
static void on_key(uint8_t key, bool pressed, uint32_t t_ms)
{
	app_post(EVT_KEY_RAW, key, pressed ? 1 : 0, t_ms);
}

/* services/buttons.c: ISR hen gio cua phim -> hang su kien */
static void on_button_timer(uint8_t idx, uint32_t gen)
{
	app_post(EVT_KEY_TIMER, idx, 0, gen);
}

static const struct buttons_cb btn_cb = {
	.post_timer = on_button_timer,
	.gesture = app_gesture,
};

/* ---------------- Hang doi su kien ---------------- */

void app_post(uint8_t type, uint8_t a, uint8_t b, uint32_t v)
{
	const struct helmet_evt evt = {.type = type, .a = a, .b = b, .v = v};

	if (k_msgq_put(&app_q, &evt, K_NO_WAIT) != 0) {
		atomic_inc(&q_overflow);
	}
}

enum helmet_state app_state(void)
{
	return state;
}

const char *app_state_str(enum helmet_state s)
{
	switch (s) {
	case HS_BOOT:
		return "BOOT";
	case HS_IDLE:
		return "IDLE";
	case HS_CONNECTED:
		return "CONNECTED";
	case HS_RINGING:
		return "RINGING";
	case HS_IN_CALL:
		return "IN_CALL";
	case HS_SHUTDOWN:
		return "SHUTDOWN";
	default:
		return "?";
	}
}

/* Kieu nhay LED theo trang thai thiet bi */
static enum status_led_pattern led_pattern(enum helmet_state s)
{
	switch (s) {
	case HS_BOOT:
		return LED_PAT_BOOT;
	case HS_IDLE:
		return LED_PAT_IDLE;
	case HS_CONNECTED:
		return LED_PAT_ON;
	case HS_RINGING:
		return LED_PAT_RING;
	case HS_IN_CALL:
		return LED_PAT_CALL;
	case HS_SHUTDOWN:
	default:
		return LED_PAT_SHUTDOWN;
	}
}

void app_set_state(enum helmet_state s)
{
	if (s == state) {
		return;
	}
	LOG_INF("State %s -> %s", app_state_str(state), app_state_str(s));
	state = s;

	status_led_set(led_pattern(s));

	/* Call: nhan dup chi co nghia khi dang ket noi (bai ke tiep) hoac dang goi (tat micro).
	 * Khi do chuong phai nghe may ngay, khong cho 350 ms.
	 */
	buttons_set_double(KEY_CALL, s == HS_CONNECTED || s == HS_IN_CALL);
	power_state_changed(s);
}

static void prompt_if_free(enum prompt_id p)
{
	/* Dang goi / do chuong: chi am bao ngan quan trong, khong chen vao giong noi */
	if (state != HS_IN_CALL && state != HS_RINGING) {
		audio_out_prompt(p);
	}
}

/* ---------------- Cuoc goi ---------------- */

static void on_call(enum helmet_call c)
{
	LOG_INF("Call state: %s", call_ctrl_state_str(c));

	switch (c) {
	case CALL_INCOMING:
		if (state == HS_CONNECTED || state == HS_IN_CALL) {
			if (state == HS_CONNECTED) {
				app_set_state(HS_RINGING);
			}
			/* Cho 1 s: dien thoai co the tu phat chuong qua LE Audio */
			k_timer_start(&ring_timer, K_MSEC(RING_CHECK_MS), K_NO_WAIT);
		}
		break;

	case CALL_ACTIVE:
	case CALL_OUTGOING:
	case CALL_HELD:
		k_timer_stop(&ring_timer);
		if (state == HS_RINGING) {
			audio_out_prompt_stop();
		}
		if (state == HS_CONNECTED || state == HS_RINGING) {
			app_set_state(HS_IN_CALL);
		}
		break;

	case CALL_NONE:
	default:
		k_timer_stop(&ring_timer);
		if (state == HS_RINGING) {
			audio_out_prompt_stop();
		}
		if (state == HS_RINGING || state == HS_IN_CALL) {
			if (voice_is_muted()) {
				(void)call_ctrl_mic_mute(false); /* cuoc sau bat dau voi micro bat */
			}
			app_set_state(app_bt_phone() ? HS_CONNECTED : HS_IDLE);
			if (temp_prompt_pending) {
				temp_prompt_pending = false;
				audio_out_prompt(PROMPT_TEMP_ALARM);
			}
		}
		break;
	}
}

/* ---------------- Xu ly su kien ---------------- */

static void handle_event(const struct helmet_evt *evt)
{
	switch (evt->type) {
	case EVT_KEY_RAW:
		if (state != HS_SHUTDOWN) {
			buttons_on_key(evt->a, evt->b != 0, evt->v);
		}
		break;

	case EVT_KEY_TIMER:
		if (state != HS_SHUTDOWN) {
			buttons_on_timer(evt->a, evt->v);
		}
		break;

	case EVT_GESTURE:
		LOG_INF("Gesture (shell) %s %s", buttons_key_str(evt->a),
			buttons_gesture_str(evt->b));
		app_gesture(evt->a, evt->b);
		break;

	case EVT_CONNECTED:
		app_set_state(HS_CONNECTED);
		break;

	case EVT_SECURED:
		audio_out_prompt(PROMPT_CONNECTED);
		break;

	case EVT_DISCONNECTED:
		k_timer_stop(&ring_timer);
		audio_out_prompt_stop();
		if (voice_is_muted()) {
			voice_set_mute(false);
		}
		app_set_state(HS_IDLE);
		audio_out_prompt(PROMPT_DISCONNECTED);
		break;

	case EVT_STREAM:
		/* Dien thoai tu phat chuong qua LE Audio -> dung chuong cuc bo */
		if (evt->a == BT_AUDIO_DIR_SINK && evt->b && state == HS_RINGING) {
			k_timer_stop(&ring_timer);
			audio_out_prompt_stop();
		}
		break;

	case EVT_CALL:
		on_call((enum helmet_call)evt->a);
		break;

	case EVT_MIC_MUTE:
		voice_set_mute(evt->a != 0);
		if (state == HS_IN_CALL) {
			audio_out_prompt(evt->a ? PROMPT_MIC_MUTED : PROMPT_MIC_UNMUTED);
		}
		break;

	case EVT_RING_CHECK:
		if (state == HS_RINGING && !le_audio_is_streaming()) {
			LOG_INF("No ringtone from phone - local ring");
			audio_out_prompt(PROMPT_RING);
		}
		break;

	case EVT_IDLE_TIMEOUT:
		/* Dang co intercom (ke ca tim / cho), Dinh vi hoac dang PHAT Auracast thi khong tu tat.
		 * Chi dang cho broadcast (nguon tam dung, chua co ma) thi van tu tat nhu IDLE.
		 */
		if (state == HS_IDLE && intercom_mode() == ICM_NORMAL && !nav_active() &&
		    bcast_mode() != BC_STREAMING) {
			power_off(OFF_IDLE);
		}
		break;

	case EVT_IC:
		intercom_handle();
		break;

	case EVT_BC:
		bcast_handle();
		break;

	case EVT_NAV:
		nav_handle();
		break;

	case EVT_ADV_SLOWDOWN:
		if (state == HS_IDLE) {
			app_adv_set_slow(true);
		}
		break;

	case EVT_TEMP_ALARM:
		if (evt->a) {
			if (state == HS_IN_CALL || state == HS_RINGING) {
				temp_prompt_pending = true;
			} else {
				audio_out_prompt(PROMPT_TEMP_ALARM);
			}
		}
		break;

	case EVT_LOW_BATTERY:
		if (evt->a == 2) {
			power_off(OFF_LOW_BATTERY);
		}
		prompt_if_free(PROMPT_LOW_BATTERY);
		break;

	case EVT_POWER_OFF_REQ:
		power_off(OFF_SHELL);
		break;

	default:
		break;
	}
}

/* ---------------- Heartbeat ---------------- */

static void heartbeat(void)
{
	struct audio_out_stats st;
	struct voice_tx_stats v;
	struct helmet_rx_flags rx;
	struct helmet_temp t;

	audio_out_get_stats(&st);
	LOG_INF("heartbeat: %s, %s, SDU %u (bad %u), PLC %u, I2S %s %u->%u.%03u Hz q%u ring %u, "
		"slip -%u/+%u, underrun %u, err %u",
		app_state_str(state), le_audio_is_streaming() ? "STREAMING" : "idle", st.sdu_rx,
		st.sdu_bad, st.plc, st.i2s_running ? "on" : "off", st.out_rate,
		st.i2s_rate_mhz / 1000U, st.i2s_rate_mhz % 1000U, st.queued_blocks, st.ring_level,
		st.slip_drop, st.slip_insert, st.underrun, st.i2s_err + st.dec_err + st.q_full);

	if (app_bt_phone() != NULL) {
		le_audio_get_rx_flags(&rx);
		LOG_INF("link: ISO rx ok %u, CRC %u, lost %u, empty %u; call %s", rx.ok, rx.crc,
			rx.lost, rx.empty, call_ctrl_state_str(call_ctrl_state()));
		le_audio_print_link_quality(NULL);
	}

	voice_get_stats(&v);
	if (v.active) {
		LOG_INF("voice: %u Hz, mic %d dBFS, out %d dBFS, ERLE %d dB, sent %u, underrun %u, "
			"slip -%u/+%u, %s",
			v.out_rate, v.mic_dbfs_x10 / 10, v.out_dbfs_x10 / 10, v.erle_db_x10 / 10,
			v.sdu_sent, v.sdu_underrun, v.slip_drop, v.slip_insert,
			v.muted ? "MUTED" : "unmuted");
	}

	thermo_get(&t);
	if (t.valid) {
		LOG_INF("temp: object %d.%02d C, ambient %d.%02d C, age %u ms%s",
			t.object_mc / 1000, (t.object_mc < 0 ? -t.object_mc : t.object_mc) % 1000 / 10,
			t.ambient_mc / 1000,
			(t.ambient_mc < 0 ? -t.ambient_mc : t.ambient_mc) % 1000 / 10, t.age_ms,
			t.alarm ? ", ALARM" : "");
	}

	intercom_log_status();
	bcast_log_status();

	if (atomic_get(&q_overflow)) {
		LOG_WRN("app queue overflow: %d events lost", (int)atomic_set(&q_overflow, 0));
	}
}

/* ---------------- main ---------------- */

int main(void)
{
	struct helmet_evt evt;
	int64_t next_hb;
	bool power_held;
	int err;

	/* Truoc tien: su kien nut tu luc khoi dong vao hang su kien (giu Power de bat may) */
	keys_init(on_key);
	power_held = power_boot_check();

	LOG_INF("=== Helmet (apps/main): music + calls + gestures + power + temperature ===");
	err = status_led_init();
	if (err) {
		LOG_WRN("Status LED init failed (%d)", err);
	}

	err = audio_out_init();
	if (err) {
		return err;
	}

	/* Khong bat buoc: loi o day chi lam mat chuc nang tuong ung */
	(void)capture_init();
	(void)mic_in_init();
	buttons_init(&btn_cb);
	voice_set_mic_unavailable_cb(call_ctrl_mic_unavailable);
	if (power_held) {
		/* Nguoi dung van dang giu Power de bat: khong de G_VERY_LONG tat may ngay */
		buttons_ignore_until_release(KEY_POWER);
	}

	/* Dang ky moi dich vu GATT dong TRUOC bt_enable(). settings_load() tinh Database Hash ngay
	 * luc nap va so voi hash da luu (gatt.c: db_hash_commit -> do_db_hash). Neu cac dich vu
	 * LE Audio dang ky SAU settings_load, hash luc nap thieu chung -> moi lan khoi dong Zephyr
	 * coi nhu DB da doi: gui Service Changed toan dai va danh dau moi dien thoai da ghep la
	 * "change-unaware" (tra loi 0x12 Database Out Of Sync) -> dien thoai ket noi roi ngat lien
	 * tuc, phai ghep lai. Zephyr chi cho dang ky truoc bt_enable() hoac sau settings_load()
	 * (bt_gatt_service_register, gatt.h). Giu nguyen thu tu cu (TMAS, VCS, MICS, PACS, ASCS)
	 * de handle khong doi -> hash khop ngay ca lan dau chay ban nay.
	 */
	err = bt_tmap_register(HELMET_TMAP_ROLES);
	if (err) {
		LOG_ERR("TMAP register failed (%d)", err);
		return err;
	}
	err = media_ctrl_register();
	if (err) {
		return err;
	}
	err = call_ctrl_register();
	if (err) {
		return err;
	}
	err = le_audio_register();
	if (err) {
		return err;
	}
	/* BASS (Auracast) dang ky sau cung: handle cac dich vu cu khong doi */
	err = bcast_register();
	if (err) {
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

	/* Nap bond, identity va cai dat helmet/... tu RRAM (phan vung storage) */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}
	helmet_cfg_loaded();
	app_bt_register_auth();
	(void)gatt_trace_init(); /* chan doan tam thoi: dien thoai doc / ghi gi */
	(void)intercom_init();
	(void)nav_init();

	err = media_ctrl_init();
	if (err) {
		return err;
	}

	err = call_ctrl_init();
	if (err) {
		return err;
	}

	err = le_audio_init();
	if (err) {
		return err;
	}
	err = bcast_init();
	if (err) {
		return err;
	}
	if (!mic_in_ready()) {
		le_audio_set_source_available(false);
		call_ctrl_mic_unavailable(true);
	}

	(void)thermo_init();

	err = app_bt_adv_create();
	if (err) {
		return err;
	}

	app_set_state(HS_IDLE);
	app_adv_refresh();
	audio_out_prompt(PROMPT_POWER_ON);

	next_hb = k_uptime_get() + HEARTBEAT_PERIOD_MS;
	while (1) {
		const int64_t now = k_uptime_get();
		const k_timeout_t to = next_hb > now ? K_MSEC(next_hb - now) : K_NO_WAIT;

		if (k_msgq_get(&app_q, &evt, to) == 0) {
			handle_event(&evt);
		}
		if (k_uptime_get() >= next_hb) {
			heartbeat();
			next_hb += HEARTBEAT_PERIOD_MS;
		}
	}

	return 0;
}
