/*
 * Bike hub (apps/bike) - trung tam am thanh tren xe.
 * Thiet ke: docs/BIKE_HUB_THIET_KE.md
 *
 *   Dien thoai --(LE Audio unicast: CIS, LC3 48 kHz)--> hub --(Auracast: BIG 2 BIS)--> moi mu
 *
 * Dien thoai ghep doi voi hub nhu mot thiet bi LE Audio thuong ("Bike_Hub", chi chieu nhan).
 * Mu (apps/main) ghep voi hub BANG NUT, khong go ma (bhs.c):
 *   hub: Power ngan -> giu Vol+ 2 s (mo cua so 60 s); mu: Power ngan -> giu Call 2 s.
 * Mu nhan broadcast ID + dia chi + ma ngau nhien, vao broadcast "Bike Audio", va tu vao lai
 * moi lan bat. So mu khong gioi han. Hub chi chuyen tiep khung LC3, khong giai ma.
 *
 * Thu tu khoi dong (giong apps/main): dang ky PACS + ASCS TRUOC bt_enable() de Database Hash
 * khong doi giua cac lan khoi dong (tranh vong lap ket noi / ngat cua dien thoai).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "bike.h"

LOG_MODULE_REGISTER(bike, LOG_LEVEL_INF);

#define HEARTBEAT_S   10
#define SEC_TIMEOUT_S 30

#define SINK_CONTEXTS_ADV                                                                          \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_MEDIA |                         \
	 BT_AUDIO_CONTEXT_TYPE_GAME | BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL)

static struct bt_le_ext_adv *adv_phone;
static struct bt_conn *phone_conn;
static bt_security_t sec_level;

static void adv_work_fn(struct k_work *w);
static K_WORK_DEFINE(adv_work, adv_work_fn);
static void sec_timeout_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sec_timeout_work, sec_timeout_fn);

/* ---------------- Quang ba cho dien thoai (co the ket noi) ---------------- */

/* ASCS: thong bao chung, nhan cac ngu canh tren, khong co chieu gui */
static const uint8_t ascs_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
	BT_BYTES_LIST_LE16(SINK_CONTEXTS_ADV),
	BT_BYTES_LIST_LE16(0),
	0x00, /* metadata length */
};

static const uint8_t cas_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
};

static const struct bt_data ad_phone[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, BT_BYTES_LIST_LE16(BT_APPEARANCE_GENERIC_AUDIO_SINK)),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_PACS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, cas_addata, ARRAY_SIZE(cas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, ascs_addata, ARRAY_SIZE(ascs_addata)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Quang ba khi chua co dien thoai. Controller tu dung khi co ket noi den. */
static void adv_work_fn(struct k_work *w)
{
	int err;

	ARG_UNUSED(w);
	if (adv_phone == NULL || phone_conn != NULL) {
		return;
	}
	err = bt_le_ext_adv_start(adv_phone, BT_LE_EXT_ADV_START_DEFAULT);
	if (err == 0) {
		LOG_INF("Advertising to phones as \"%s\"", CONFIG_BT_DEVICE_NAME);
	} else if (err != -EALREADY) {
		LOG_WRN("Phone advertising start failed (%d)", err);
	}
}

static int adv_phone_create(void)
{
	int err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_CONN_QUICK, NULL, &adv_phone);

	if (err) {
		LOG_ERR("Phone advertising set create failed (%d)", err);
		return err;
	}
	err = bt_le_ext_adv_set_data(adv_phone, ad_phone, ARRAY_SIZE(ad_phone), NULL, 0);
	if (err) {
		LOG_ERR("Phone advertising data failed (%d)", err);
	}
	return err;
}

/* ---------------- Ket noi / bao mat ---------------- */

/* Ket noi den co the la dien thoai hoac mu dang ghep (qua bo quang ba cua so ghep). Mu duoc
 * nhan ra khi doc Join Info (hub_on_helmet); truoc do tam coi la dien thoai.
 */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err) {
		LOG_WRN("Connection from %s failed (0x%02x %s)", addr, err, bt_hci_err_to_str(err));
		k_work_submit(&adv_work);
		return;
	}
	if (phone_conn == NULL) {
		phone_conn = bt_conn_ref(conn);
		sec_level = BT_SECURITY_L1;
	}
	LOG_INF("Connected: %s", addr);

	/* LE Audio yeu cau ma hoa */
	if (bt_conn_set_security(conn, BT_SECURITY_L2) != 0) {
		LOG_WRN("Security request failed");
	}
	k_work_reschedule(&sec_timeout_work, K_SECONDS(SEC_TIMEOUT_S));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (bhs_is_helmet(conn)) {
		LOG_INF("Helmet %s disconnected (reason 0x%02x)", addr, reason);
		bhs_conn_disconnected(conn);
		return;
	}
	LOG_INF("Disconnected: %s (reason 0x%02x %s)", addr, reason, bt_hci_err_to_str(reason));
	if (conn == phone_conn) {
		(void)k_work_cancel_delayable(&sec_timeout_work);
		bt_conn_unref(phone_conn);
		phone_conn = NULL;
		if (!bhs_pair_active()) {
			hub_tone(HUB_TONE_DISCONNECTED); /* trong cua so: thuong la mu thu ghep lai */
		}
	}
}

/* BHS: ket noi nay la mu (vua doc Join Info) chu khong phai dien thoai */
void hub_on_helmet(struct bt_conn *conn)
{
	if (conn == phone_conn) {
		(void)k_work_cancel_delayable(&sec_timeout_work);
		bt_conn_unref(phone_conn);
		phone_conn = NULL;
		k_work_submit(&adv_work); /* tiep tuc cho dien thoai */
	}
}

static void recycled(void)
{
	k_work_submit(&adv_work); /* o bt_conn vua trong: quang ba lai cho dien thoai */
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		if (bhs_pair_active()) {
			/* Cua so ghep mo: co the la mu mang khoa cu -> khong ngat de mu thay dung loi
			 * ("key missing") va tu xoa khoa + ghep lai. Dien thoai khong ma hoa duoc van bi
			 * ngat sau SEC_TIMEOUT_S; ke la khong doc duoc Join Info (can ma hoa).
			 */
			LOG_WRN("Security failed (level %d, err %d %s) - pairing window open, "
				"letting peer re-pair",
				level, err, bt_security_err_to_str(err));
			return;
		}
		LOG_WRN("Security failed (level %d, err %d %s) - disconnecting", level, err,
			bt_security_err_to_str(err));
		(void)k_work_cancel_delayable(&sec_timeout_work);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	if (conn != phone_conn) {
		return; /* mu dang ghep: BHS xu ly */
	}
	(void)k_work_cancel_delayable(&sec_timeout_work);
	sec_level = level;
	LOG_INF("Link encrypted (level %d)", level);
	if (!bhs_pair_active()) {
		hub_tone(HUB_TONE_CONNECTED); /* trong cua so ghep co the la mu: khong bao */
	}
}

/* Ket noi khong ma hoa sau 30 s (vd may khong ghep doi): ngat de dien thoai that ket noi duoc */
static void sec_timeout_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (phone_conn != NULL && sec_level < BT_SECURITY_L2 && !bhs_is_helmet(phone_conn)) {
		LOG_WRN("Not encrypted after %d s - disconnecting", SEC_TIMEOUT_S);
		(void)bt_conn_disconnect(phone_conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	LOG_INF("Pairing complete (%s)", bonded ? "bonded" : "not bonded");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("Pairing failed: %d %s - forget \"%s\" on the phone and pair again", reason,
		bt_security_err_to_str(reason), CONFIG_BT_DEVICE_NAME);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ---------------- Shell ---------------- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char addr[BT_ADDR_LE_STR_LEN] = "-";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (phone_conn != NULL) {
		bt_addr_le_to_str(bt_conn_get_dst(phone_conn), addr, sizeof(addr));
	}
	shell_print(sh, "Phone     : %s%s (security %d)", addr,
		    phone_conn ? "" : " (advertising)", phone_conn ? sec_level : 0);
	phone_sink_print(sh);
	bcast_source_print(sh);
	bhs_print(sh);
	return 0;
}

static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bhs_pair_start();
	shell_print(sh, "Helmet pairing window open 60 s (same as Power short -> hold Vol+ 2 s)");
	return 0;
}

static int cmd_code(const struct shell *sh, size_t argc, char **argv)
{
	enum bike_code_mode mode = BIKE_CODE_TEXT;
	int err;

	ARG_UNUSED(argc);
	if (strcmp(argv[1], "new") == 0) {
		mode = BIKE_CODE_RANDOM;
	} else if (strcmp(argv[1], "open") == 0) {
		mode = BIKE_CODE_OPEN;
	}
	err = bcast_source_set_code(mode, mode == BIKE_CODE_TEXT ? argv[1] : NULL);
	if (err == -EINVAL) {
		shell_error(sh, "usage: bike code new | open | <text up to 16 chars>");
		return err;
	}
	shell_print(sh, "Broadcast %s - rebuilding. Paired helmets must pair again (Call button)",
		    mode == BIKE_CODE_OPEN ? "is now open (no code)" : "code changed");
	return 0;
}

static int cmd_rtn(const struct shell *sh, size_t argc, char **argv)
{
	const long r = strtol(argv[1], NULL, 10);

	ARG_UNUSED(argc);
	if (r < 0 || r > 4 || bcast_source_set_rtn((uint8_t)r) == -EINVAL) {
		shell_error(sh, "usage: bike rtn <0..4>");
		return -EINVAL;
	}
	shell_print(sh, "BIS retransmissions = %ld - rebuilding broadcast", r);
	return 0;
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (phone_conn != NULL) {
		(void)bt_conn_disconnect(phone_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	shell_print(sh, "%s", err ? "Unpair failed" : "All phone bonds deleted");
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bike_cmds,
	SHELL_CMD(status, NULL, "Phone link, ASEs, broadcast and relay statistics", cmd_status),
	SHELL_CMD(pair, NULL, "Open helmet pairing window 60 s", cmd_pair),
	SHELL_CMD_ARG(code, NULL, "Broadcast code: bike code new | open | <text>", cmd_code, 2, 0),
	SHELL_CMD_ARG(rtn, NULL, "BIS retransmissions: bike rtn <0..4> (default 2)", cmd_rtn, 2,
		      0),
	SHELL_CMD(unpair, NULL, "Delete all phone bonds", cmd_unpair),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bike, &bike_cmds, "Bike audio hub", NULL);

/* ---------------- Nut ---------------- */

static void unpair_all(void)
{
	bhs_pair_cancel();
	if (phone_conn != NULL) {
		(void)bt_conn_disconnect(phone_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	if (bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY) == 0) {
		LOG_INF("All bonds deleted (phone + helmets)");
		hub_tone(HUB_TONE_UNPAIR);
	}
}

/* Thread hub_ui */
void hub_gesture(enum hub_gesture g)
{
	switch (g) {
	case HUB_G_STATUS:
		if (bhs_pair_active()) {
			bhs_pair_cancel(); /* Power ngan khi dang ghep: dong cua so */
		} else {
			hub_tone(phone_conn ? HUB_TONE_STATUS_OK : HUB_TONE_STATUS_NO_LINK);
		}
		break;
	case HUB_G_POWER_OFF:
		hub_power_off();
		break;
	case HUB_G_PAIR:
		bhs_pair_start();
		break;
	case HUB_G_UNPAIR:
		unpair_all();
		break;
	case HUB_G_CALL:
		LOG_INF("Call button: not used yet");
		break;
	default:
		break;
	}
}

static void disconnect_cb(struct bt_conn *conn, void *data)
{
	ARG_UNUSED(data);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_POWER_OFF);
}

void hub_bt_shutdown(void)
{
	bhs_pair_cancel();
	if (adv_phone != NULL) {
		(void)bt_le_ext_adv_stop(adv_phone);
	}
	bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_cb, NULL);
	k_msleep(300);
}

/* ---------------- main ---------------- */

int main(void)
{
	int err;

	(void)hub_power_boot_check(); /* thuc tu System OFF: phai giu Power 2 s */
	LOG_INF("=== Bike hub (apps/bike): phone LE Audio -> Auracast broadcast to helmets ===");
	(void)hub_ui_init();

	/* GATT dong (PACS, ASCS) truoc bt_enable(): xem chu thich dau file */
	err = phone_sink_register();
	if (err) {
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load(); /* bond, identity, bike/bid, bike/code, bike/rtn */
	}
	bt_conn_auth_info_cb_register(&auth_info_cb);

	err = phone_sink_init();
	if (err) {
		return err;
	}
	err = bcast_source_init();
	if (err) {
		LOG_ERR("Broadcast init failed (%d)", err);
		return err;
	}
	err = bhs_init();
	if (err) {
		return err;
	}
	err = adv_phone_create();
	if (err) {
		return err;
	}
	k_work_submit(&adv_work);
	hub_tone(HUB_TONE_POWER_ON);

	while (1) {
		k_sleep(K_SECONDS(HEARTBEAT_S));
		LOG_INF("heartbeat: phone %s, %s", phone_conn ? "connected" : "advertising",
			phone_sink_streaming() ? "STREAMING" : "idle");
		bcast_source_log_status();
	}
	return 0;
}
