/*
 * Helmet (apps/main) - lop UNG DUNG: quang ba LE Audio, ket noi / bao mat, giao ket noi cho luong
 * dien thoai. Ket noi cua intercom va Bike hub duoc chuyen cho chuc nang tuong ung.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "app/app_internal.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/phone/call_ctrl.h"
#include "features/phone/le_audio.h"
#include "features/phone/media_ctrl.h"
#include "features/thermo/thermo.h"

LOG_MODULE_DECLARE(helmet, LOG_LEVEL_INF);

/* Ket noi ma chua ma hoa sau ngan nay giay -> ngat (du thoi gian cho hop thoai "Ghep doi?"
 * cua Android o lan dau)
 */
#define SEC_TIMEOUT_S 30

static struct bt_le_ext_adv *adv;
static bool adv_slow;    /* che do quang ba mong muon: true = 150 ms */
static bool adv_on;      /* bo quang ba dang chay that */
static bool adv_on_slow; /* ... va dang o che do cham */
static bool adv_data_ic; /* du lieu quang ba dang co co intercom (slave dang cho) */
static struct bt_conn *active_conn;
static bt_security_t sec_level;
static K_SEM_DEFINE(disc_sem, 0, 1);

static void adv_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_work, adv_work_handler);
static void sec_timeout_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sec_timeout_work, sec_timeout_handler);

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
	const bool want = adv != NULL && app_state() != HS_SHUTDOWN && (active_conn == NULL || ic);
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

int app_bt_adv_create(void)
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
	app_set_state(HS_SHUTDOWN);
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
	if (app_state() != HS_SHUTDOWN && app_state() != HS_BOOT) {
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

void app_bt_register_auth(void)
{
	bt_conn_auth_info_cb_register(&auth_info_cb);
}

struct bt_conn *app_bt_phone(void)
{
	return active_conn;
}

bt_security_t app_bt_sec_level(void)
{
	return sec_level;
}

bool app_bt_adv_slow(void)
{
	return adv_slow;
}
