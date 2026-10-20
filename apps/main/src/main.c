/*
 * Helmet (apps/main) - chuong trinh chinh: mu bao hiem LE Audio day du chuc nang.
 * Thiet ke: docs/HELMET_THIET_KE_CHUC_NANG_MOI.md
 *
 * Bo dieu khien ung dung: moi su kien (nut, ket noi, cuoc goi, luong am thanh, hen gio,
 * canh bao nhiet, pin) tu cac thread khac duoc dang vao app_q; thread main xu ly tuan tu
 * -> khong can khoa, mot may trang thai thiet bi duy nhat:
 *
 *   BOOT -> IDLE (quang ba) <-> CONNECTED <-> RINGING -> IN_CALL -> CONNECTED
 *                 \___________ Power giu 3 s / tu tat ___________/-> SHUTDOWN -> System OFF
 *
 * Vai tro Bluetooth: CAP Acceptor + BAP Unicast Server (2 sink + 1 source), TMAP UMR + CT,
 * VCP Volume Renderer, MCP client, CCP client (GTBS), MICP Microphone Device, HTS.
 * Log + shell qua RTT (make rtt) va UART20: lenh "helmet ...".
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/tmap.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "helmet.h"

LOG_MODULE_REGISTER(helmet, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS 10000
#define APP_Q_LEN           24
#define RING_CHECK_MS       1000
/* Ket noi ma chua ma hoa sau ngan nay giay -> ngat (du thoi gian cho hop thoai "Ghep doi?"
 * cua Android o lan dau)
 */
#define SEC_TIMEOUT_S       30
/* To hop intercom: sau Power nhan ngan co 2 s de bat dau giu Vol+ (master) / Vol- (slave) */
#define IC_SEQ_WINDOW_MS    2000

K_MSGQ_DEFINE(app_q, sizeof(struct helmet_evt), APP_Q_LEN, 4);

static enum helmet_state state = HS_BOOT;
static struct bt_le_ext_adv *adv;
static bool adv_slow;    /* che do quang ba mong muon: true = 150 ms */
static bool adv_on;      /* bo quang ba dang chay that */
static bool adv_on_slow; /* ... va dang o che do cham */
static bool adv_data_ic; /* du lieu quang ba dang co co intercom (slave dang cho) */
static struct bt_conn *active_conn;
static bt_security_t sec_level;
static K_SEM_DEFINE(disc_sem, 0, 1);
static atomic_t q_overflow;
static bool temp_prompt_pending;

static void adv_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_work, adv_work_handler);
static void sec_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sec_timeout_work, sec_timeout_handler);

static void ring_check_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	app_post(EVT_RING_CHECK, 0, 0, 0);
}
static K_TIMER_DEFINE(ring_timer, ring_check_expiry, NULL);

/* ---------------- Cai dat luu qua lan tat ---------------- */

struct helmet_cfg helmet_cfg = {
	.volume = 128,
	.autooff_min = 10,
	.temp_emis_permille = 980,
	.temp_alarm_mc = 38000,
};
static struct helmet_cfg saved_cfg;

static int cfg_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	void *dst = NULL;
	size_t size = 0;

	if (settings_name_steq(key, "vol", &next) && !next) {
		dst = &helmet_cfg.volume;
		size = sizeof(helmet_cfg.volume);
	} else if (settings_name_steq(key, "autooff", &next) && !next) {
		dst = &helmet_cfg.autooff_min;
		size = sizeof(helmet_cfg.autooff_min);
	} else if (settings_name_steq(key, "emis", &next) && !next) {
		dst = &helmet_cfg.temp_emis_permille;
		size = sizeof(helmet_cfg.temp_emis_permille);
	} else if (settings_name_steq(key, "alarm", &next) && !next) {
		dst = &helmet_cfg.temp_alarm_mc;
		size = sizeof(helmet_cfg.temp_alarm_mc);
	} else {
		return -ENOENT;
	}

	if (len != size || read_cb(cb_arg, dst, size) != (ssize_t)size) {
		return -EINVAL;
	}
	return 0;
}

static int cfg_commit(void)
{
	saved_cfg = helmet_cfg;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(helmet, "helmet", NULL, cfg_set, cfg_commit, NULL);

void helmet_cfg_save(void)
{
	if (helmet_cfg.volume != saved_cfg.volume) {
		(void)settings_save_one("helmet/vol", &helmet_cfg.volume, sizeof(helmet_cfg.volume));
	}
	if (helmet_cfg.autooff_min != saved_cfg.autooff_min) {
		(void)settings_save_one("helmet/autooff", &helmet_cfg.autooff_min,
					sizeof(helmet_cfg.autooff_min));
	}
	if (helmet_cfg.temp_emis_permille != saved_cfg.temp_emis_permille) {
		(void)settings_save_one("helmet/emis", &helmet_cfg.temp_emis_permille,
					sizeof(helmet_cfg.temp_emis_permille));
	}
	if (helmet_cfg.temp_alarm_mc != saved_cfg.temp_alarm_mc) {
		(void)settings_save_one("helmet/alarm", &helmet_cfg.temp_alarm_mc,
					sizeof(helmet_cfg.temp_alarm_mc));
	}
	saved_cfg = helmet_cfg;
}

static void cfg_save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	helmet_cfg_save();
}
static K_WORK_DELAYABLE_DEFINE(cfg_save_work, cfg_save_work_fn);

void helmet_cfg_changed(void)
{
	/* Gom nhieu thay doi (vd giu Vol+) thanh 1 lan ghi RRAM */
	k_work_reschedule(&cfg_save_work, K_SECONDS(10));
}

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

static void set_state(enum helmet_state s)
{
	if (s == state) {
		return;
	}
	LOG_INF("State %s -> %s", app_state_str(state), app_state_str(s));
	state = s;

	status_led_set_state(s);

	/* Call: nhan dup chi co nghia khi dang ket noi (bai ke tiep) hoac dang goi (tat micro).
	 * Khi do chuong phai nghe may ngay, khong cho 350 ms.
	 */
	buttons_set_double(KEY_CALL, s == HS_CONNECTED || s == HS_IN_CALL);
	power_state_changed(s);
}

/* ---------------- Quang ba LE Audio ---------------- */

/* ASCS: ngu canh nhan (nhac, goi, chuong...) va gui (micro: cuoc goi) */
static const uint8_t ascs_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
	BT_BYTES_LIST_LE16(HELMET_SINK_CONTEXTS),
	BT_BYTES_LIST_LE16(HELMET_SOURCE_CONTEXTS),
	0x00, /* metadata length */
};

static const uint8_t cas_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
};

/* TMAP: nghe nhac unicast (UMR), goi dien (CT), nghe broadcast Auracast (BMR) */
#define HELMET_TMAP_ROLES (BT_TMAP_ROLE_UMR | BT_TMAP_ROLE_CT | BT_TMAP_ROLE_BMR)

static const uint8_t tmas_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_TMAS_VAL),
	BT_BYTES_LIST_LE16(HELMET_TMAP_ROLES),
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      BT_BYTES_LIST_LE16(BT_APPEARANCE_WEARABLE_AUDIO_DEVICE_HEADSET)),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_TMAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, tmas_addata, ARRAY_SIZE(tmas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, cas_addata, ARRAY_SIZE(cas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, ascs_addata, ARRAY_SIZE(ascs_addata)),
	/* Scan Delegator: Broadcast Assistant (dien thoai) nhan ra mu nghe duoc Auracast */
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Slave intercom dang cho: them Service Data 128 bit cua HIS de master nhan ra */
static const uint8_t ic_addata[] = {IC_UUID_SVC_VAL, IC_PROTO_VER, IC_ADV_FLAG_SLAVE};

static const struct bt_data ad_ic[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      BT_BYTES_LIST_LE16(BT_APPEARANCE_WEARABLE_AUDIO_DEVICE_HEADSET)),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_TMAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, tmas_addata, ARRAY_SIZE(tmas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, cas_addata, ARRAY_SIZE(cas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, ascs_addata, ARRAY_SIZE(ascs_addata)),
	/* Scan Delegator: Broadcast Assistant (dien thoai) nhan ra mu nghe duoc Auracast */
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_BASS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA128, ic_addata, ARRAY_SIZE(ic_addata)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_work_handler(struct k_work *work)
{
	/* Quang ba khi chua co dien thoai, hoac khi la slave intercom dang cho (ca khi da co dien
	 * thoai: BT_MAX_CONN = 3). Slave dang cho luon quang ba nhanh.
	 */
	const bool ic = intercom_adv_ic();
	const bool want = adv != NULL && state != HS_SHUTDOWN && (active_conn == NULL || ic);
	const bool slow = adv_slow && !ic;
	int err;

	ARG_UNUSED(work);

	if (!want) {
		/* Chi dung khi dang chay (vd het che do slave trong luc co dien thoai). Dung ->
		 * recycled() -> work nay -> !want va adv_on = false -> ket thuc, khong lap.
		 */
		if (adv != NULL && adv_on) {
			(void)bt_le_ext_adv_stop(adv);
			adv_on = false;
			LOG_INF("Advertising stopped");
		}
		return;
	}

	if (ic != adv_data_ic) {
		err = bt_le_ext_adv_set_data(adv, ic ? ad_ic : ad, ic ? ARRAY_SIZE(ad_ic) : ARRAY_SIZE(ad),
					     NULL, 0);
		if (err) {
			LOG_ERR("Advertising data update failed (%d)", err);
		} else {
			adv_data_ic = ic;
		}
	}

	/* Dang quang ba dung che do mong muon -> khong lam gi.
	 * KHONG duoc dung quang ba khi khong can: dung bo quang ba co the ket noi se tra lai o
	 * bt_conn dang cho ket noi -> Zephyr goi recycled() -> recycled() gui lai work nay
	 * -> vong lap vo han chiem het CPU (da gap tren mach).
	 */
	if (adv_on && adv_on_slow == slow) {
		return;
	}

	if (adv_on) {
		(void)bt_le_ext_adv_stop(adv); /* doi che do: phai dung truoc khi doi tham so */
		adv_on = false;
	}

	err = bt_le_ext_adv_update_param(adv, slow ? BT_BAP_ADV_PARAM_CONN_REDUCED
						   : BT_BAP_ADV_PARAM_CONN_QUICK);
	if (err) {
		LOG_WRN("Advertising param update failed (%d)", err);
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		LOG_ERR("Advertising start failed (%d)", err);
		return;
	}
	adv_on = true;
	adv_on_slow = slow;
	LOG_INF("Advertising as \"%s\" (%s%s)", CONFIG_BT_DEVICE_NAME,
		slow ? "reduced 150 ms" : "fast 20-30 ms", ic ? ", intercom slave" : "");
}

void app_adv_set_slow(bool slow)
{
	adv_slow = slow;
	k_work_submit(&adv_work);
}

void app_adv_refresh(void)
{
	k_work_submit(&adv_work);
}

static int advertising_create(void)
{
	int err;

	err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_CONN_QUICK, NULL, &adv);
	if (err) {
		LOG_ERR("Advertising set create failed (%d)", err);
		return err;
	}

	err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising data failed (%d)", err);
		return err;
	}

	return 0;
}

void app_bt_shutdown(void)
{
	struct bt_conn *conn = active_conn ? bt_conn_ref(active_conn) : NULL;

	bcast_leave();       /* roi broadcast Auracast (dung BIG, PA) */
	intercom_shutdown(); /* bao END cho mu ban truoc khi ngat */
	set_state(HS_SHUTDOWN);
	if (conn != NULL) {
		k_sem_reset(&disc_sem);
		if (bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_POWER_OFF) == 0) {
			(void)k_sem_take(&disc_sem, K_SECONDS(1));
		}
		bt_conn_unref(conn);
	}
	if (adv != NULL) {
		(void)bt_le_ext_adv_stop(adv);
		adv_on = false;
	}
}

/* ---------------- Ket noi / bao mat ---------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct bt_conn_info info;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	/* Controller tu dung bo quang ba khi co ket noi DEN (peripheral). Ket noi central do
	 * intercom master tao khong anh huong bo quang ba.
	 */
	if (bt_conn_get_info(conn, &info) != 0 || info.role == BT_CONN_ROLE_PERIPHERAL) {
		adv_on = false;
	}
	/* Lien ket tam toi Bike hub (lay thong tin vao xe): hub_link.c xu ly */
	if (hub_link_owned(conn)) {
		hub_link_connected(conn, err);
		return;
	}
	/* Ket noi mu-mu (master) hoac ung vien khi slave dang cho: intercom.c xu ly */
	if (intercom_conn_connected(conn, err)) {
		if (!err) {
			LOG_INF("Connected: %s (intercom)", addr);
		}
		return;
	}
	if (err) {
		LOG_WRN("Connection to %s failed (0x%02x %s)", addr, err, bt_hci_err_to_str(err));
		return;
	}

	if (active_conn == NULL) {
		active_conn = bt_conn_ref(conn);
	}
	sec_level = BT_SECURITY_L1;
	LOG_INF("Connected: %s", addr);
	app_post(EVT_CONNECTED, 0, 0, 0);

	/* LE Audio yeu cau lien ket ma hoa: chu dong de nghi ghep doi/ma hoa */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Security request failed (%d)", err);
	}
	/* May kia im lang (khong ghep doi, cung khong tu choi) -> sec_timeout_handler ngat */
	k_work_reschedule(&sec_timeout_work, K_SECONDS(SEC_TIMEOUT_S));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason 0x%02x %s)", addr, reason, bt_hci_err_to_str(reason));

	if (hub_link_owned(conn)) {
		hub_link_disconnected(conn, reason);
		return;
	}
	if (intercom_conn_owned(conn)) {
		intercom_conn_disconnected(conn, reason);
		return;
	}

	if (conn == active_conn) {
		(void)k_work_cancel_delayable(&sec_timeout_work);
		bt_conn_unref(active_conn);
		active_conn = NULL;
		adv_slow = false; /* mat ket noi that: quang ba nhanh lai 30 s */
		media_ctrl_disconnected();
		call_ctrl_disconnected();
		app_post(EVT_DISCONNECTED, reason, 0, 0);
	}
	k_sem_give(&disc_sem);
}

/* Zephyr goi khi mot o bt_conn trong kho vua trong - KHONG chi sau khi mat ket noi, ma ca
 * khi chinh ung dung dung bo quang ba co the ket noi. Vi vay o day chi "nhac" bat quang ba;
 * adv_work_handler tu bo qua neu dang quang ba dung che do (khong gay vong lap).
 */
static void recycled(void)
{
	if (state != HS_SHUTDOWN && state != HS_BOOT) {
		k_work_submit(&adv_work);
	}
}

/* Ket noi SEC_TIMEOUT_S giay ma van chua ma hoa -> ngat. Moi dich vu LE Audio deu bat ma hoa,
 * va chi co 1 o ket noi (BT_MAX_CONN = 1): giu ket noi vo dung nay thi dien thoai that khong
 * tim thay Helmet, hen gio tu tat cung khong chay (chi chay o IDLE).
 */
static void sec_timeout_handler(struct k_work *work)
{
	/* sysworkq va bt_rx deu la thread cooperative: active_conn khong doi giua 2 dong duoi */
	struct bt_conn *conn = active_conn ? bt_conn_ref(active_conn) : NULL;

	ARG_UNUSED(work);

	if (conn == NULL) {
		return;
	}
	if (bt_conn_get_security(conn) < BT_SECURITY_L2) {
		LOG_WRN("Not encrypted after %d s - disconnecting (0x%02x)", SEC_TIMEOUT_S,
			BT_HCI_ERR_AUTH_FAIL);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
	bt_conn_unref(conn);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (hub_link_owned(conn)) {
		hub_link_secured(conn, level, err);
		return;
	}
	if (intercom_conn_owned(conn)) {
		intercom_conn_secured(conn, level, err);
		return;
	}
	if (err) {
		/* Zephyr chi bao loi, khong tu ngat: ngat la chinh sach cua ung dung. May kia tu
		 * choi ghep doi, hoac ma hoa bang khoa cu ma Helmet da xoa -> ngat ngay de quang
		 * ba lai cho dien thoai khac.
		 */
		LOG_WRN("Security failed: level %d, err %d %s - disconnecting (0x%02x)", level, err,
			bt_security_err_to_str(err), BT_HCI_ERR_AUTH_FAIL);
		(void)k_work_cancel_delayable(&sec_timeout_work);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	(void)k_work_cancel_delayable(&sec_timeout_work);
	sec_level = level;
	LOG_INF("Link encrypted (security level %d)", level);
	app_post(EVT_SECURED, 0, 0, 0);
	/* Do MCS (play/pause) roi tiep GTBS (cuoc goi) - noi tiep trong media_ctrl */
	media_ctrl_connected(conn);
}

static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(conn);
	LOG_INF("ACL PHY: tx %s, rx %s", le_audio_phy_str(param->tx_phy),
		le_audio_phy_str(param->rx_phy));
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("ACL params: interval %u us, latency %u, timeout %u ms", interval * 1250U, latency,
		timeout * 10U);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
	.le_phy_updated = le_phy_updated,
	.le_param_updated = le_param_updated,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	LOG_INF("Pairing complete (%s)", bonded ? "bonded, keys saved" : "not bonded");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("Pairing failed: %d %s - xoa thiet bi tren dien thoai roi ghep lai", reason,
		bt_security_err_to_str(reason));
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* Ket noi ma intercom (slave dang cho) nhan lam ung vien nhung khong phai mu ban: chuyen
 * sang luong dien thoai. Chay trong sysworkq (cooperative, nhu bt_rx) de doi active_conn an
 * toan voi cac callback Bluetooth.
 */
K_MSGQ_DEFINE(adopt_q, sizeof(struct bt_conn *), 2, 4);

static void adopt_work_fn(struct k_work *work)
{
	struct bt_conn *conn;
	struct bt_conn_info info;

	ARG_UNUSED(work);

	while (k_msgq_get(&adopt_q, &conn, K_NO_WAIT) == 0) {
		intercom_conn_release(conn);
		if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED) {
			bt_conn_unref(conn);
			continue;
		}
		if (active_conn != NULL) {
			LOG_INF("Extra connection (not a helmet, phone already connected) - disconnecting");
			(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			bt_conn_unref(conn);
			continue;
		}
		active_conn = conn; /* giu ref */
		sec_level = bt_conn_get_security(conn);
		LOG_INF("Connection handed over to phone handling (security %d)", sec_level);
		app_post(EVT_CONNECTED, 0, 0, 0);
		if (sec_level >= BT_SECURITY_L2) {
			app_post(EVT_SECURED, 0, 0, 0);
			media_ctrl_connected(conn);
		} else {
			k_work_reschedule(&sec_timeout_work, K_SECONDS(SEC_TIMEOUT_S));
		}
	}
}

static K_WORK_DEFINE(adopt_work, adopt_work_fn);

void app_phone_adopt(struct bt_conn *conn)
{
	if (k_msgq_put(&adopt_q, &conn, K_NO_WAIT) != 0) {
		intercom_conn_release(conn);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
		return;
	}
	k_work_submit(&adopt_work);
}

/* ---------------- Nut nhan: gpio-keys -> su kien tho ---------------- */

static void on_input(struct input_event *evt, void *user_data)
{
	int key;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
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
	app_post(EVT_KEY_RAW, (uint8_t)key, evt->value ? 1 : 0, k_uptime_get_32());
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* ---------------- Hanh dong theo cu chi (docs/HELMET_THIET_KE_CHUC_NANG_MOI.md muc 5.6) */

static void prompt_if_free(enum helmet_prompt p)
{
	/* Dang goi / do chuong: chi am bao ngan quan trong, khong chen vao giong noi */
	if (state != HS_IN_CALL && state != HS_RINGING) {
		audio_out_prompt(p);
	}
}

static void factory_unpair(void)
{
	int err;

	LOG_WRN("Vol+ + Vol- held 5 s: delete all bonds");
	if (active_conn != NULL) {
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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

	if (state == HS_SHUTDOWN || state == HS_BOOT) {
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
		if (state != HS_RINGING && state != HS_IN_CALL && intercom_mode() != ICM_NORMAL) {
			if (g == G_SHORT) {
				intercom_stop();
			}
			break;
		}
		if (state == HS_RINGING) {
			if (g == G_SHORT) {
				err = call_ctrl_accept();
			} else if (g == G_LONG) {
				err = call_ctrl_reject();
			}
		} else if (state == HS_IN_CALL) {
			if (g == G_SHORT) {
				err = call_ctrl_hangup();
			} else if (g == G_DOUBLE) {
				err = call_ctrl_mic_mute(!voice_is_muted());
			}
		} else if (state == HS_CONNECTED) {
			if (g == G_SHORT) {
				err = media_ctrl_play_pause();
			} else if (g == G_DOUBLE) {
				err = media_ctrl_next_track();
			}
		} else if (state == HS_IDLE && g == G_SHORT) {
			audio_out_prompt(PROMPT_STATUS_NO_LINK);
		}
		break;

	case KEY_VOL_UP:
		if (g == G_SEQ_HOLD) {
			intercom_select(IC_ROLE_MASTER); /* Power ngan -> giu Vol+ 2 s */
		} else if (g == G_SHORT || g == G_REPEAT) {
			err = media_ctrl_volume_up();
		} else if (g == G_COMBO_VOL && (state == HS_IDLE || state == HS_CONNECTED)) {
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
			audio_out_prompt((active_conn || m == ICM_LINKED) ? PROMPT_STATUS_OK
									 : PROMPT_STATUS_NO_LINK);
			/* Buoc 1 cua to hop chon vai tro intercom */
			if (m == ICM_NORMAL && (state == HS_IDLE || state == HS_CONNECTED)) {
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

/* ---------------- Cuoc goi ---------------- */

static void on_call(enum helmet_call c)
{
	LOG_INF("Call state: %s", call_ctrl_state_str(c));

	switch (c) {
	case CALL_INCOMING:
		if (state == HS_CONNECTED || state == HS_IN_CALL) {
			if (state == HS_CONNECTED) {
				set_state(HS_RINGING);
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
			set_state(HS_IN_CALL);
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
			set_state(active_conn ? HS_CONNECTED : HS_IDLE);
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
	case EVT_KEY_TIMER:
		if (state != HS_SHUTDOWN) {
			buttons_handle(evt);
		}
		break;

	case EVT_GESTURE:
		LOG_INF("Gesture (shell) %s %s", buttons_key_str(evt->a),
			buttons_gesture_str(evt->b));
		app_gesture(evt->a, evt->b);
		break;

	case EVT_CONNECTED:
		set_state(HS_CONNECTED);
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
		set_state(HS_IDLE);
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
		/* Dang co intercom (ke ca tim / cho) hoac dang PHAT Auracast thi khong tu tat.
		 * Chi dang cho broadcast (nguon tam dung, chua co ma) thi van tu tat nhu IDLE.
		 */
		if (state == HS_IDLE && intercom_mode() == ICM_NORMAL &&
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
	struct helmet_audio_stats st;
	struct helmet_voice_stats v;
	struct helmet_rx_flags rx;
	struct helmet_temp t;

	audio_out_get_stats(&st);
	LOG_INF("heartbeat: %s, %s, SDU %u (bad %u), PLC %u, I2S %s %u->%u.%03u Hz q%u ring %u, "
		"slip -%u/+%u, underrun %u, err %u",
		app_state_str(state), le_audio_is_streaming() ? "STREAMING" : "idle", st.sdu_rx,
		st.sdu_bad, st.plc, st.i2s_running ? "on" : "off", st.out_rate,
		st.i2s_rate_mhz / 1000U, st.i2s_rate_mhz % 1000U, st.queued_blocks, st.ring_level,
		st.slip_drop, st.slip_insert, st.underrun, st.i2s_err + st.dec_err + st.q_full);

	if (active_conn != NULL) {
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

/* ---------------- Shell ---------------- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char addr[BT_ADDR_LE_STR_LEN] = "-";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (active_conn != NULL) {
		bt_addr_le_to_str(bt_conn_get_dst(active_conn), addr, sizeof(addr));
	}
	shell_print(sh, "State     : %s", app_state_str(state));
	shell_print(sh, "Connection: %s%s (security %d)", addr,
		    active_conn ? "" : (adv_slow ? " (advertising slow)" : " (advertising)"),
		    active_conn ? sec_level : 0);
	media_ctrl_print_status(sh);
	call_ctrl_print_status(sh);
	le_audio_print_status(sh);
	bcast_print(sh);
	voice_print_status(sh);
	power_print_status(sh);
	return 0;
}

static int cmd_vol(const struct shell *sh, size_t argc, char **argv)
{
	const long v = strtol(argv[1], NULL, 10);

	if (v < 0 || v > 255) {
		shell_error(sh, "usage: helmet vol <0..255>");
		return -EINVAL;
	}
	return media_ctrl_set_volume((uint8_t)v);
}

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_play();
}

static int cmd_pause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_pause();
}

static int cmd_next(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_next_track();
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (err) {
		shell_error(sh, "unpair failed (%d)", err);
		return err;
	}
	shell_print(sh, "All bonds deleted - xoa ca \"%s\" tren dien thoai truoc khi ghep lai",
		    CONFIG_BT_DEVICE_NAME);
	return 0;
}

static int cmd_capture(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			capture_set_enabled(true);
		} else if (strcmp(argv[1], "off") == 0) {
			capture_set_enabled(false);
		} else {
			shell_error(sh, "usage: helmet capture [on|off]");
			return -EINVAL;
		}
	}
	capture_print_status(sh);
	return 0;
}

static int parse_phy(const char *s, uint8_t *phy)
{
	if (strcmp(s, "1m") == 0) {
		*phy = BT_GAP_LE_PHY_1M;
	} else if (strcmp(s, "2m") == 0) {
		*phy = BT_GAP_LE_PHY_2M;
	} else if (strcmp(s, "coded") == 0) {
		*phy = BT_GAP_LE_PHY_CODED;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int cmd_cispref(const struct shell *sh, size_t argc, char **argv)
{
	const long rtn = strtol(argv[2], NULL, 10);
	const long latency = strtol(argv[3], NULL, 10);
	uint8_t phy;

	if (parse_phy(argv[1], &phy) != 0 || phy == BT_GAP_LE_PHY_CODED || rtn < 0 || rtn > 15 ||
	    latency < 5 || latency > 4000) {
		shell_error(sh, "usage: helmet cispref <1m|2m> <rtn 0..15> <latency_ms 5..4000>");
		return -EINVAL;
	}
	le_audio_set_qos_pref(phy, (uint8_t)rtn, (uint16_t)latency);
	return 0;
}

static int cmd_btn(const struct shell *sh, size_t argc, char **argv)
{
	const int key = buttons_key_from_str(argv[1]);
	const int g = buttons_gesture_from_str(argv[2]);

	if (key < 0 || g < 0) {
		shell_error(sh, "usage: helmet btn <volup|voldown|call|power> "
				"<short|double|long|vlong|repeat|combo>");
		return -EINVAL;
	}
	app_post(EVT_GESTURE, (uint8_t)key, (uint8_t)g, 0);
	return 0;
}

static int cmd_call(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;

	if (argc > 1) {
		if (strcmp(argv[1], "accept") == 0) {
			err = call_ctrl_accept();
		} else if (strcmp(argv[1], "reject") == 0) {
			err = call_ctrl_reject();
		} else if (strcmp(argv[1], "end") == 0) {
			err = call_ctrl_hangup();
		} else {
			shell_error(sh, "usage: helmet call [accept|reject|end]");
			return -EINVAL;
		}
		if (err) {
			shell_error(sh, "failed (%d)", err);
		}
	}
	call_ctrl_print_status(sh);
	return err;
}

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "mute") == 0) {
			return call_ctrl_mic_mute(true);
		} else if (strcmp(argv[1], "unmute") == 0) {
			return call_ctrl_mic_mute(false);
		}
		shell_error(sh, "usage: helmet mic [mute|unmute]");
		return -EINVAL;
	}
	shell_print(sh, "Microphone %s", voice_is_muted() ? "MUTED" : "unmuted");
	return 0;
}

static int cmd_voice(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	voice_print_status(sh);
	return 0;
}

/* helmet dsp <aec|ns|agc> <on|off> */
static int cmd_dsp(const struct shell *sh, size_t argc, char **argv)
{
	bool aec;
	bool ns;
	bool agc;
	bool on;

	voice_get_dsp(&aec, &ns, &agc);
	if (argc < 3) {
		shell_print(sh, "AEC %s, NS %s, AGC %s", aec ? "on" : "off", ns ? "on" : "off",
			    agc ? "on" : "off");
		return 0;
	}
	on = strcmp(argv[2], "on") == 0;
	if (strcmp(argv[1], "aec") == 0) {
		aec = on;
	} else if (strcmp(argv[1], "ns") == 0) {
		ns = on;
	} else if (strcmp(argv[1], "agc") == 0) {
		agc = on;
	} else {
		shell_error(sh, "usage: helmet dsp <aec|ns|agc> <on|off>");
		return -EINVAL;
	}
	voice_set_dsp(aec, ns, agc);
	shell_print(sh, "AEC %s, NS %s, AGC %s", aec ? "on" : "off", ns ? "on" : "off",
		    agc ? "on" : "off");
	return 0;
}

static int cmd_prompt(const struct shell *sh, size_t argc, char **argv)
{
	const int p = prompt_from_name(argv[1]);

	if (strcmp(argv[1], "stop") == 0) {
		audio_out_prompt_stop();
		return 0;
	}
	if (p < 0) {
		shell_error(sh, "unknown prompt; one of:");
		for (int i = 0; i < PROMPT_COUNT; i++) {
			shell_print(sh, "  %s", prompt_name(i));
		}
		return -EINVAL;
	}
	if (p == PROMPT_RING) {
		shell_print(sh, "ring repeats - stop with: helmet prompt stop");
	}
	audio_out_prompt((enum helmet_prompt)p);
	return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "off") == 0) {
		app_post(EVT_POWER_OFF_REQ, 0, 0, 0);
		return 0;
	}
	power_print_status(sh);
	return 0;
}

static int cmd_autooff(const struct shell *sh, size_t argc, char **argv)
{
	const long v = strtol(argv[1], NULL, 10);

	if (v < 0 || v > 240) {
		shell_error(sh, "usage: helmet autooff <minutes 0..240> (0 = never)");
		return -EINVAL;
	}
	helmet_cfg.autooff_min = (uint16_t)v;
	helmet_cfg_changed();
	power_autooff_changed();
	shell_print(sh, "Auto power off: %ld min", v);
	return 0;
}

static int cmd_ic(const struct shell *sh, size_t argc, char **argv)
{
	return intercom_shell(sh, argc, argv);
}

static int cmd_trace(const struct shell *sh, size_t argc, char **argv)
{
	return gatt_trace_shell(sh, argc, argv);
}

static int cmd_bc(const struct shell *sh, size_t argc, char **argv)
{
	return bcast_shell(sh, argc, argv);
}

static int cmd_perf(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	audio_out_print_perf(sh);
	voice_print_perf(sh);
	return 0;
}

/* CPU that cua tung thread trong mot cua so thoi gian (CONFIG_THREAD_RUNTIME_STATS).
 * Khac "helmet perf" (do thoi gian tu dau toi cuoi, ke ca luc bi thread khac chen ngang).
 * Thoi gian ngat duoc tinh vao thread dang chay luc ngat xay ra.
 */
#define CPU_MAX_THREADS 24
#define CPU_WINDOW_MS   2000

struct cpu_snap {
	const struct k_thread *t;
	uint64_t cyc;
};

struct cpu_ctx {
	struct cpu_snap s[CPU_MAX_THREADS];
	uint8_t n;
};

static void cpu_collect(const struct k_thread *t, void *user_data)
{
	struct cpu_ctx *c = user_data;
	k_thread_runtime_stats_t rs;

	if (c->n < CPU_MAX_THREADS && k_thread_runtime_stats_get((k_tid_t)t, &rs) == 0) {
		c->s[c->n].t = t;
		c->s[c->n].cyc = rs.execution_cycles;
		c->n++;
	}
}

static int cmd_cpu(const struct shell *sh, size_t argc, char **argv)
{
	static struct cpu_ctx before;
	static struct cpu_ctx after;
	const uint32_t ms = (argc > 1) ? strtoul(argv[1], NULL, 10) : CPU_WINDOW_MS;
	uint64_t window;
	uint64_t busy = 0;

	if (ms < 100 || ms > 60000) {
		shell_error(sh, "Window: 100..60000 ms");
		return -EINVAL;
	}

	before.n = 0;
	k_thread_foreach_unlocked(cpu_collect, &before);
	k_msleep(ms);
	after.n = 0;
	k_thread_foreach_unlocked(cpu_collect, &after);

	window = (uint64_t)ms * sys_clock_hw_cycles_per_sec() / MSEC_PER_SEC;
	shell_print(sh, "CPU over %u ms (128 MHz):", ms);
	for (int i = 0; i < after.n; i++) {
		const char *name = k_thread_name_get((k_tid_t)after.s[i].t);
		uint64_t d = after.s[i].cyc;
		uint32_t pct_x10;

		for (int j = 0; j < before.n; j++) {
			if (before.s[j].t == after.s[i].t) {
				d -= before.s[j].cyc;
				break;
			}
		}
		pct_x10 = (uint32_t)(d * 1000U / window);
		if (name == NULL || name[0] == '\0') {
			name = "?";
		}
		if (strcmp(name, "idle") != 0) {
			busy += d;
		}
		if (pct_x10 > 0) {
			shell_print(sh, "  %-20s %3u.%u %%", name, pct_x10 / 10U, pct_x10 % 10U);
		}
	}
	busy = busy * 1000U / window;
	shell_print(sh, "  %-20s %3u.%u %%", "TOTAL (non-idle)", (uint32_t)busy / 10U,
		    (uint32_t)busy % 10U);
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	return thermo_shell(sh, argc, argv);
}

static int cmd_vdd(const struct shell *sh, size_t argc, char **argv)
{
	const int mv = power_vdd_mv();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (mv < 0) {
		shell_error(sh, "VDD not available (%d)", mv);
		return mv;
	}
	shell_print(sh, "VDD %d mV", mv);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(helmet_cmds,
	SHELL_CMD(status, NULL, "State, connection, volume, calls, streams, voice, power",
		  cmd_status),
	SHELL_CMD_ARG(vol, NULL, "Set volume: helmet vol <0..255>", cmd_vol, 2, 0),
	SHELL_CMD(play, NULL, "Send PLAY to phone", cmd_play),
	SHELL_CMD(pause, NULL, "Send PAUSE to phone", cmd_pause),
	SHELL_CMD(next, NULL, "Send NEXT TRACK to phone", cmd_next),
	SHELL_CMD(unpair, NULL, "Delete all bonds", cmd_unpair),
	SHELL_CMD_ARG(capture, NULL,
		      "Send received LC3 audio to PC over RTT channel 1: helmet capture [on|off]",
		      cmd_capture, 1, 1),
	SHELL_CMD_ARG(cispref, NULL,
		      "Test: preferred CIS QoS: helmet cispref <1m|2m> <rtn> <latency_ms>",
		      cmd_cispref, 4, 0),
	SHELL_CMD_ARG(btn, NULL,
		      "Simulate gesture: helmet btn <volup|voldown|call|power> "
		      "<short|double|long|vlong|repeat|combo>",
		      cmd_btn, 3, 0),
	SHELL_CMD_ARG(call, NULL, "Calls: helmet call [accept|reject|end]", cmd_call, 1, 1),
	SHELL_CMD_ARG(mic, NULL, "Microphone: helmet mic [mute|unmute]", cmd_mic, 1, 1),
	SHELL_CMD(voice, NULL, "Uplink (microphone) statistics", cmd_voice),
	SHELL_CMD_ARG(dsp, NULL, "Voice DSP: helmet dsp [<aec|ns|agc> <on|off>]", cmd_dsp, 1, 2),
	SHELL_CMD_ARG(prompt, NULL, "Play prompt: helmet prompt <name>|stop", cmd_prompt, 2, 0),
	SHELL_CMD_ARG(power, NULL, "Power status, or: helmet power off", cmd_power, 1, 1),
	SHELL_CMD_ARG(autooff, NULL, "Auto power off: helmet autooff <minutes> (0 = never)",
		      cmd_autooff, 2, 0),
	SHELL_CMD(perf, NULL, "CPU time of decode / output / voice DSP / encode", cmd_perf),
	SHELL_CMD_ARG(cpu, NULL, "CPU load per thread: helmet cpu [window_ms] (default 2000)",
		      cmd_cpu, 1, 1),
	SHELL_CMD_ARG(ic, NULL,
		      "Intercom: helmet ic [status|master|slave|stop|unpair|tone <Hz>|off]", cmd_ic,
		      1, 2),
	SHELL_CMD_ARG(bc, NULL,
		      "Auracast: helmet bc [status|scan [name]|stop|forget|code <text>|code clear]", cmd_bc,
		      1, 2),
	SHELL_CMD_ARG(trace, NULL, "Diagnostic GATT access log: helmet trace [on|off]", cmd_trace,
		      1, 1),
	SHELL_CMD_ARG(temp, NULL,
		      "Temperature: helmet temp [raw|cal|emis <permille>|alarm <C>]", cmd_temp,
		      1, 2),
	SHELL_CMD(vdd, NULL, "Measure supply voltage (VDD)", cmd_vdd),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(helmet, &helmet_cmds, "Helmet LE Audio headset", NULL);

/* ---------------- main ---------------- */

int main(void)
{
	const bool power_held = power_boot_check();
	struct helmet_evt evt;
	int64_t next_hb;
	int err;

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
	buttons_init();
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
	saved_cfg = helmet_cfg;
	bt_conn_auth_info_cb_register(&auth_info_cb);
	(void)gatt_trace_init(); /* chan doan tam thoi: dien thoai doc / ghi gi */
	(void)intercom_init();

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

	err = advertising_create();
	if (err) {
		return err;
	}

	set_state(HS_IDLE);
	k_work_submit(&adv_work);
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
