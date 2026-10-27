/*
 * Helmet - dieu khien cuoc goi.
 *
 * - CCP Call Control Client qua TBS client: do GTBS (Generic Telephone Bearer Service) tren
 *   dien thoai, nhan danh sach cuoc goi {index, state, flags} moi khi dien thoai thong bao,
 *   gui lenh nghe / ket thuc. "Tu choi" = ket thuc cuoc goi dang do chuong.
 * - MICP Microphone Device: dien thoai tat/bat micro cua mu; nut tren mu cung di qua
 *   bt_micp_mic_dev_mute/unmute -> mic_mute_cb (mot duong xu ly duy nhat).
 *
 * Callback chay trong thread Bluetooth: chi chep du lieu roi dang su kien len bo dieu khien.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/micp.h>
#include <zephyr/bluetooth/audio/tbs.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <me54/services/audio_out.h>

#include "app/app.h"
#include "features/phone/call_ctrl.h"

LOG_MODULE_REGISTER(call_ctrl, LOG_LEVEL_INF);

#define MAX_CALLS CONFIG_BT_TBS_CLIENT_MAX_CALLS

static struct bt_conn *call_conn;
static bool gtbs_ready;
static struct bt_tbs_client_call_state calls[MAX_CALLS];
static uint8_t call_count;
static enum helmet_call agg = CALL_NONE;
static uint8_t mic_mute = BT_MICP_MUTE_UNMUTED;
static uint8_t last_reason = 0xFF;

static void discover_work_handler(struct k_work *work);
static K_WORK_DEFINE(discover_work, discover_work_handler);

static const char *tbs_state_str(uint8_t state)
{
	switch (state) {
	case BT_TBS_CALL_STATE_INCOMING:
		return "INCOMING";
	case BT_TBS_CALL_STATE_DIALING:
		return "DIALING";
	case BT_TBS_CALL_STATE_ALERTING:
		return "ALERTING";
	case BT_TBS_CALL_STATE_ACTIVE:
		return "ACTIVE";
	case BT_TBS_CALL_STATE_LOCALLY_HELD:
	case BT_TBS_CALL_STATE_REMOTELY_HELD:
	case BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD:
		return "HELD";
	default:
		return "?";
	}
}

static const char *reason_str(uint8_t reason)
{
	switch (reason) {
	case BT_TBS_REASON_REMOTE_ENDED_CALL:
		return "remote ended";
	case BT_TBS_REASON_SERVER_ENDED_CALL:
		return "phone ended";
	case BT_TBS_REASON_LINE_BUSY:
		return "line busy";
	case BT_TBS_REASON_NETWORK_CONGESTED:
		return "network congested";
	case BT_TBS_REASON_CLIENT_TERMINATED:
		return "ended by headset";
	case BT_TBS_REASON_NO_SERVICE:
		return "no service";
	case BT_TBS_REASON_NO_ANSWER:
		return "no answer";
	case BT_TBS_REASON_CALL_FAILED:
		return "call failed";
	default:
		return "unspecified";
	}
}

const char *call_ctrl_state_str(enum helmet_call c)
{
	switch (c) {
	case CALL_NONE:
		return "none";
	case CALL_INCOMING:
		return "incoming";
	case CALL_OUTGOING:
		return "outgoing";
	case CALL_ACTIVE:
		return "active";
	case CALL_HELD:
		return "held";
	default:
		return "?";
	}
}

/* Trang thai tong hop, uu tien: INCOMING > ACTIVE > DIALING/ALERTING > HELD > NONE */
static enum helmet_call aggregate(void)
{
	bool active = false;
	bool outgoing = false;
	bool held = false;

	for (uint8_t i = 0; i < call_count; i++) {
		switch (calls[i].state) {
		case BT_TBS_CALL_STATE_INCOMING:
			return CALL_INCOMING;
		case BT_TBS_CALL_STATE_ACTIVE:
			active = true;
			break;
		case BT_TBS_CALL_STATE_DIALING:
		case BT_TBS_CALL_STATE_ALERTING:
			outgoing = true;
			break;
		default:
			held = true;
			break;
		}
	}
	return active ? CALL_ACTIVE : outgoing ? CALL_OUTGOING : held ? CALL_HELD : CALL_NONE;
}

static int find_call(uint8_t s1, uint8_t s2, uint8_t s3, uint8_t *index)
{
	const uint8_t want[3] = {s1, s2, s3};

	for (uint8_t w = 0; w < ARRAY_SIZE(want); w++) {
		for (uint8_t i = 0; i < call_count; i++) {
			if (calls[i].state == want[w]) {
				*index = calls[i].index;
				return 0;
			}
		}
	}
	return -ENOENT;
}

/* ---------------- TBS client callbacks ---------------- */

static void discover_cb(struct bt_conn *conn, int err, uint8_t tbs_count, bool gtbs_found)
{
	if (err || !gtbs_found) {
		LOG_WRN("GTBS not found on phone (err %d) - call control disabled", err);
		gtbs_ready = false;
		return;
	}
	gtbs_ready = true;
	LOG_INF("GTBS found (%u TBS instances) - call control ready", tbs_count);
}

static void call_state_cb(struct bt_conn *conn, int err, uint8_t inst_index, uint8_t count,
			  const struct bt_tbs_client_call_state *states)
{
	enum helmet_call next;

	if (err || inst_index != BT_TBS_GTBS_INDEX) {
		return;
	}

	call_count = MIN(count, MAX_CALLS);
	for (uint8_t i = 0; i < call_count; i++) {
		calls[i] = states[i];
		LOG_INF("Call #%u %s%s", states[i].index, tbs_state_str(states[i].state),
			(states[i].flags & BT_TBS_CALL_FLAG_OUTGOING) ? " (outgoing)" : "");
	}

	next = aggregate();
	if (next != agg) {
		agg = next;
		app_post(EVT_CALL, (uint8_t)agg, 0, 0);
	}
}

static void termination_reason_cb(struct bt_conn *conn, int err, uint8_t inst_index,
				  uint8_t call_index, uint8_t reason)
{
	if (err) {
		return;
	}
	last_reason = reason;
	LOG_INF("Call #%u ended (reason 0x%02x %s)", call_index, reason, reason_str(reason));
}

static void cp_result(const char *what, int err, uint8_t call_index)
{
	if (err) {
		LOG_WRN("%s call #%u failed (%d)", what, call_index, err);
		audio_out_prompt(PROMPT_VOL_LIMIT); /* tieng "tic": lenh khong thanh cong */
	} else {
		LOG_INF("%s call #%u OK", what, call_index);
	}
}

static void accept_cb(struct bt_conn *conn, int err, uint8_t inst_index, uint8_t call_index)
{
	cp_result("Accept", err, call_index);
}

static void terminate_cb(struct bt_conn *conn, int err, uint8_t inst_index, uint8_t call_index)
{
	cp_result("Terminate", err, call_index);
}

#if defined(CONFIG_BT_TBS_CLIENT_INCOMING_CALL)
static void remote_uri_cb(struct bt_conn *conn, int err, uint8_t inst_index, const char *value)
{
	if (!err && value != NULL) {
		LOG_INF("Incoming call from %s", value);
	}
}
#endif

#if defined(CONFIG_BT_TBS_CLIENT_CALL_FRIENDLY_NAME)
static void friendly_name_cb(struct bt_conn *conn, int err, uint8_t inst_index, const char *value)
{
	if (!err && value != NULL) {
		LOG_INF("Caller name: %s", value);
	}
}
#endif

static struct bt_tbs_client_cb tbs_cb = {
	.discover = discover_cb,
	.accept_call = accept_cb,
	.terminate_call = terminate_cb,
	.call_state = call_state_cb,
	.termination_reason = termination_reason_cb,
#if defined(CONFIG_BT_TBS_CLIENT_INCOMING_CALL)
	.remote_uri = remote_uri_cb,
#endif
#if defined(CONFIG_BT_TBS_CLIENT_CALL_FRIENDLY_NAME)
	.friendly_name = friendly_name_cb,
#endif
};

static void discover_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (call_conn == NULL) {
		return;
	}
	err = bt_tbs_client_discover(call_conn);
	if (err) {
		LOG_WRN("GTBS discovery start failed (%d)", err);
	}
}

/* ---------------- MICP Microphone Device ---------------- */

static void mic_mute_cb(uint8_t mute)
{
	if (mute == mic_mute) {
		return;
	}
	mic_mute = mute;
	LOG_INF("Microphone %s", mute == BT_MICP_MUTE_MUTED      ? "MUTED"
				 : mute == BT_MICP_MUTE_DISABLED ? "DISABLED"
								 : "unmuted");
	app_post(EVT_MIC_MUTE, mute == BT_MICP_MUTE_MUTED ? 1 : 0, 0, 0);
}

static struct bt_micp_mic_dev_cb micp_cb = {
	.mute = mic_mute_cb,
};

/* ---------------- API ---------------- */

/* Goi TRUOC bt_enable(): chi them MICS vao co so du lieu GATT (xem app/app_ctrl.c) */
int call_ctrl_register(void)
{
	struct bt_micp_mic_dev_register_param micp_param = {
		.cb = &micp_cb,
	};
	int err;

	err = bt_micp_mic_dev_register(&micp_param);
	if (err) {
		LOG_ERR("MICP register failed (%d)", err);
	}
	return err;
}

/* Goi sau bt_enable() + settings_load() */
int call_ctrl_init(void)
{
	int err;

	err = bt_tbs_client_register_cb(&tbs_cb);
	if (err) {
		LOG_ERR("TBS client register failed (%d)", err);
		return err;
	}

	return 0;
}

void call_ctrl_connected(struct bt_conn *conn)
{
	if (call_conn != NULL) {
		return;
	}
	call_conn = bt_conn_ref(conn);
	gtbs_ready = false;
	call_count = 0;
	agg = CALL_NONE;
	k_work_submit(&discover_work);
}

void call_ctrl_disconnected(void)
{
	if (call_conn != NULL) {
		bt_conn_unref(call_conn);
		call_conn = NULL;
	}
	gtbs_ready = false;
	call_count = 0;
	agg = CALL_NONE;
}

static int check_ready(void)
{
	if (call_conn == NULL || !gtbs_ready) {
		LOG_WRN("No call control on phone");
		return -ENOTCONN;
	}
	return 0;
}

int call_ctrl_accept(void)
{
	uint8_t index;
	int err = check_ready();

	if (err) {
		return err;
	}
	if (find_call(BT_TBS_CALL_STATE_INCOMING, BT_TBS_CALL_STATE_INCOMING,
		      BT_TBS_CALL_STATE_INCOMING, &index) != 0) {
		return -ENOENT;
	}
	LOG_INF("Accept call #%u", index);
	return bt_tbs_client_accept_call(call_conn, BT_TBS_GTBS_INDEX, index);
}

int call_ctrl_reject(void)
{
	uint8_t index;
	int err = check_ready();

	if (err) {
		return err;
	}
	if (find_call(BT_TBS_CALL_STATE_INCOMING, BT_TBS_CALL_STATE_INCOMING,
		      BT_TBS_CALL_STATE_INCOMING, &index) != 0) {
		return -ENOENT;
	}
	LOG_INF("Reject call #%u", index);
	return bt_tbs_client_terminate_call(call_conn, BT_TBS_GTBS_INDEX, index);
}

int call_ctrl_hangup(void)
{
	uint8_t index;
	int err = check_ready();

	if (err) {
		return err;
	}
	/* Cuoc dang noi chuyen, neu khong co thi cuoc dang quay so, cuoi cung la cuoc dang giu */
	if (find_call(BT_TBS_CALL_STATE_ACTIVE, BT_TBS_CALL_STATE_ALERTING,
		      BT_TBS_CALL_STATE_DIALING, &index) != 0 &&
	    find_call(BT_TBS_CALL_STATE_LOCALLY_HELD, BT_TBS_CALL_STATE_REMOTELY_HELD,
		      BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD, &index) != 0) {
		return -ENOENT;
	}
	LOG_INF("Hang up call #%u", index);
	return bt_tbs_client_terminate_call(call_conn, BT_TBS_GTBS_INDEX, index);
}

int call_ctrl_mic_mute(bool mute)
{
	return mute ? bt_micp_mic_dev_mute() : bt_micp_mic_dev_unmute();
}

void call_ctrl_mic_unavailable(bool unavailable)
{
	if (unavailable) {
		(void)bt_micp_mic_dev_mute_disable();
	} else if (mic_mute == BT_MICP_MUTE_DISABLED) {
		(void)bt_micp_mic_dev_unmute();
	}
}

enum helmet_call call_ctrl_state(void)
{
	return agg;
}

void call_ctrl_print_status(const struct shell *sh)
{
	shell_print(sh, "Calls     : %s, GTBS %s, %u call(s), last end reason %s",
		    call_ctrl_state_str(agg), gtbs_ready ? "ready" : "not available", call_count,
		    last_reason == 0xFF ? "-" : reason_str(last_reason));
	for (uint8_t i = 0; i < call_count; i++) {
		shell_print(sh, "  call #%u : %s%s", calls[i].index, tbs_state_str(calls[i].state),
			    (calls[i].flags & BT_TBS_CALL_FLAG_OUTGOING) ? " (outgoing)" : "");
	}
	shell_print(sh, "Mic mute  : %s", mic_mute == BT_MICP_MUTE_MUTED      ? "muted"
					  : mic_mute == BT_MICP_MUTE_DISABLED ? "disabled"
									      : "unmuted");
}
