/*
 * Helmet - chuong trinh chinh: mu bao hiem nghe nhac qua Bluetooth LE Audio.
 *
 * nRF54L15 chi co Bluetooth LE, KHONG co Bluetooth Classic, nen khong the dung A2DP.
 * Nhac duoc truyen bang LE Audio: dien thoai ho tro LE Audio ket noi toi Helmet,
 * gui luong LC3 qua kenh dang thoi (CIS); Helmet giai ma va phat ra 2 loa qua I2S.
 *
 * Vai tro Bluetooth cua Helmet:
 *   - CAP Acceptor + BAP Unicast Server (nhan am thanh)
 *   - TMAP Unicast Media Receiver (UMR)
 *   - VCP Volume Renderer (dien thoai va nut tren mu cung chinh am luong)
 *   - MCP client (nut Call -> play/pause tren dien thoai)
 *
 * Nut (board nut): Vol+ / Vol- : am luong, Call: play/pause, Power: (du phong)
 * Log + shell qua RTT (make rtt) va UART20. Lenh: "helmet status|vol|play|pause|unpair".
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
#include <zephyr/net_buf.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "helmet.h"

LOG_MODULE_REGISTER(helmet, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_S 10

static struct bt_le_ext_adv *adv;
static struct bt_conn *active_conn;
static bt_security_t sec_level;

static void adv_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_work, adv_work_handler);

/* ---------------- Quang ba LE Audio ---------------- */

/* ASCS: "san sang nhan am thanh" cho cac ngu canh HELMET_SINK_CONTEXTS */
static const uint8_t ascs_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
	BT_BYTES_LIST_LE16(HELMET_SINK_CONTEXTS),
	BT_BYTES_LIST_LE16(0x0000), /* khong co nguon am thanh (chua dung micro) */
	0x00,                       /* metadata length */
};

static const uint8_t cas_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
	BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL,
};

static const uint8_t tmas_addata[] = {
	BT_UUID_16_ENCODE(BT_UUID_TMAS_VAL),
	BT_BYTES_LIST_LE16(BT_TMAP_ROLE_UMR),
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      BT_BYTES_LIST_LE16(BT_APPEARANCE_WEARABLE_AUDIO_DEVICE_HEADSET)),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_TMAS_VAL)),
	BT_DATA(BT_DATA_SVC_DATA16, tmas_addata, ARRAY_SIZE(tmas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, cas_addata, ARRAY_SIZE(cas_addata)),
	BT_DATA(BT_DATA_SVC_DATA16, ascs_addata, ARRAY_SIZE(ascs_addata)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (active_conn != NULL || adv == NULL) {
		return;
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		LOG_ERR("Advertising start failed (%d)", err);
		return;
	}
	LOG_INF("Advertising as \"%s\" (LE Audio headset)", CONFIG_BT_DEVICE_NAME);
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

/* ---------------- Ket noi / bao mat ---------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err) {
		LOG_WRN("Connection to %s failed (0x%02x %s)", addr, err, bt_hci_err_to_str(err));
		return;
	}

	if (active_conn == NULL) {
		active_conn = bt_conn_ref(conn);
	}
	sec_level = BT_SECURITY_L1;
	LOG_INF("Connected: %s", addr);

	/* LE Audio yeu cau lien ket ma hoa: chu dong de nghi ghep doi/ma hoa */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Security request failed (%d)", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason 0x%02x %s)", addr, reason, bt_hci_err_to_str(reason));

	if (conn == active_conn) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
		media_ctrl_disconnected();
	}
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		LOG_WRN("Security failed: level %d, err %d %s", level, err,
			bt_security_err_to_str(err));
		return;
	}

	sec_level = level;
	LOG_INF("Link encrypted (security level %d)", level);
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

/* ---------------- Chan doan lien ket (PHY, chu ky ACL, co ISO) ---------------- */

static void log_link(void)
{
	struct bt_conn *conn = active_conn ? bt_conn_ref(active_conn) : NULL;
	struct helmet_rx_flags rx;
	struct bt_conn_info info;
	const char *tx_phy = "?";
	const char *rx_phy = "?";
	uint32_t interval_us = 0;

	if (conn == NULL) {
		return;
	}

	if (bt_conn_get_info(conn, &info) == 0) {
		interval_us = info.le.interval_us;
		if (info.le.phy != NULL) {
			tx_phy = le_audio_phy_str(info.le.phy->tx_phy);
			rx_phy = le_audio_phy_str(info.le.phy->rx_phy);
		}
	}
	bt_conn_unref(conn);

	/* SDC khong ho tro HCI Read RSSI -> chi bao PHY + chu ky ACL */
	le_audio_get_rx_flags(&rx);
	LOG_INF("link: ACL interval %u us, PHY tx %s / rx %s; ISO rx ok %u, CRC %u, lost %u, "
		"empty %u",
		interval_us, tx_phy, rx_phy, rx.ok, rx.crc, rx.lost, rx.empty);
	le_audio_print_link_quality(NULL);
}

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

/* ---------------- Nut nhan ---------------- */

static void on_input(struct input_event *evt, void *user_data)
{
	int err = 0;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return; /* chi xu ly luc nhan xuong */
	}

	switch (evt->code) {
	case INPUT_KEY_VOLUMEUP:
		LOG_INF("Button Vol+");
		err = media_ctrl_volume_up();
		break;
	case INPUT_KEY_VOLUMEDOWN:
		LOG_INF("Button Vol-");
		err = media_ctrl_volume_down();
		break;
	case INPUT_KEY_PLAYPAUSE:
		LOG_INF("Button Call -> play/pause");
		err = media_ctrl_play_pause();
		break;
	case INPUT_KEY_POWER:
		LOG_INF("Button Power (chua gan chuc nang)");
		break;
	default:
		break;
	}

	if (err) {
		LOG_WRN("Button action failed (%d)", err);
	}
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* ---------------- Shell ---------------- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char addr[BT_ADDR_LE_STR_LEN] = "-";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (active_conn != NULL) {
		bt_addr_le_to_str(bt_conn_get_dst(active_conn), addr, sizeof(addr));
	}
	shell_print(sh, "Connection: %s%s (security %d)", addr,
		    active_conn ? "" : " (advertising)", active_conn ? sec_level : 0);
	media_ctrl_print_status(sh);
	le_audio_print_status(sh);
	return 0;
}

static int cmd_vol(const struct shell *sh, size_t argc, char **argv)
{
	long v = strtol(argv[1], NULL, 10);

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

/* Thu nghiem: doi PHY cua lien ket ACL (kiem tra Helmet nhan 2M co tot khong) */
static int cmd_aclphy(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_conn_le_phy_param param = {.options = BT_CONN_LE_PHY_OPT_NONE};
	uint8_t phy;
	int err;

	if (parse_phy(argv[1], &phy) != 0) {
		shell_error(sh, "usage: helmet aclphy <1m|2m|coded>");
		return -EINVAL;
	}
	if (active_conn == NULL) {
		shell_error(sh, "not connected");
		return -ENOTCONN;
	}

	param.pref_tx_phy = phy;
	param.pref_rx_phy = phy;
	err = bt_conn_le_phy_update(active_conn, &param);
	if (err) {
		shell_error(sh, "PHY update failed (%d)", err);
	}
	return err;
}

/* Thu nghiem: xin doi chu ky ACL. Controller Zephyr (ll_sw) lam central co the tinh sai
 * CIS offset khi chu ky ACL khac chu ky ISO -> dat ACL = ISO interval (10 ms) roi tao lai luong.
 */
static int cmd_aclint(const struct shell *sh, size_t argc, char **argv)
{
	long ms = strtol(argv[1], NULL, 10);
	struct bt_le_conn_param param;
	uint16_t units;
	int err;

	/* Don vi 1.25 ms: chi nhan so ms chia het cho 5 (10, 15, 20, ...) */
	if (ms < 10 || ms > 4000 || (ms * 4) % 5 != 0) {
		shell_error(sh, "usage: helmet aclint <ms>  (10, 15, 20, ..., boi so cua 5)");
		return -EINVAL;
	}
	if (active_conn == NULL) {
		shell_error(sh, "not connected");
		return -ENOTCONN;
	}

	units = (uint16_t)((ms * 4) / 5);
	param = (struct bt_le_conn_param)BT_LE_CONN_PARAM_INIT(units, units, 0, 400);
	err = bt_conn_le_param_update(active_conn, &param);
	if (err) {
		shell_error(sh, "param update failed (%d)", err);
	} else {
		shell_print(sh, "Requested ACL interval %ld ms - dung roi phat lai nhac de tao lai CIS",
			    ms);
	}
	return err;
}

/* Thu nghiem: QoS Helmet de nghi cho CIS (PHY, so lan phat lai, do tre). Thiet bi phat
 * co the khong theo; xem dong "QoS ASE" / "CIS ASE" sau khi phat lai nhac.
 */
static int cmd_cispref(const struct shell *sh, size_t argc, char **argv)
{
	long rtn = strtol(argv[2], NULL, 10);
	long latency = strtol(argv[3], NULL, 10);
	uint8_t phy;

	if (parse_phy(argv[1], &phy) != 0 || phy == BT_GAP_LE_PHY_CODED || rtn < 0 || rtn > 15 ||
	    latency < 5 || latency > 4000) {
		shell_error(sh, "usage: helmet cispref <1m|2m> <rtn 0..15> <latency_ms 5..4000>");
		return -EINVAL;
	}
	le_audio_set_qos_pref(phy, (uint8_t)rtn, (uint16_t)latency);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(helmet_cmds,
	SHELL_CMD(status, NULL, "Connection, volume, streams, audio statistics", cmd_status),
	SHELL_CMD_ARG(vol, NULL, "Set volume: helmet vol <0..255>", cmd_vol, 2, 0),
	SHELL_CMD(play, NULL, "Send PLAY to phone", cmd_play),
	SHELL_CMD(pause, NULL, "Send PAUSE to phone", cmd_pause),
	SHELL_CMD(unpair, NULL, "Delete all bonds", cmd_unpair),
	SHELL_CMD_ARG(capture, NULL,
		      "Send received LC3 audio to PC over RTT channel 1: helmet capture [on|off]"
		      " (used by scripts/helmet_audio.py)",
		      cmd_capture, 1, 1),
	SHELL_CMD_ARG(aclphy, NULL, "Test: change ACL link PHY: helmet aclphy <1m|2m|coded>",
		      cmd_aclphy, 2, 0),
	SHELL_CMD_ARG(aclint, NULL, "Test: request ACL connection interval: helmet aclint <ms>",
		      cmd_aclint, 2, 0),
	SHELL_CMD_ARG(cispref, NULL,
		      "Test: preferred CIS QoS for next stream config: "
		      "helmet cispref <1m|2m> <rtn> <latency_ms>",
		      cmd_cispref, 4, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(helmet, &helmet_cmds, "Helmet LE Audio headset", NULL);

/* ---------------- main ---------------- */

int main(void)
{
	struct helmet_audio_stats st;
	int err;

	LOG_INF("=== Helmet LE Audio headset (ME54BS62 / nRF54L15) ===");

	err = audio_out_init();
	if (err) {
		return err;
	}

	/* Khong bat buoc: loi o day chi lam mat chuc nang nghe lai qua J-Link */
	(void)capture_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

	/* Nap bond + identity tu RRAM (phan vung storage) */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}
	bt_conn_auth_info_cb_register(&auth_info_cb);

	err = bt_tmap_register(BT_TMAP_ROLE_UMR);
	if (err) {
		LOG_ERR("TMAP register failed (%d)", err);
		return err;
	}

	err = media_ctrl_init();
	if (err) {
		return err;
	}

	err = le_audio_init();
	if (err) {
		return err;
	}

	err = advertising_create();
	if (err) {
		return err;
	}
	k_work_submit(&adv_work);

	while (1) {
		k_sleep(K_SECONDS(HEARTBEAT_PERIOD_S));

		audio_out_get_stats(&st);
		LOG_INF("heartbeat: %s, %s, SDU %u (bad %u), PLC %u, I2S %s %u Hz q%u, "
			"sync -%u/+%u, err %u",
			active_conn ? "connected" : "advertising",
			le_audio_is_streaming() ? "STREAMING" : "idle", st.sdu_rx, st.sdu_bad,
			st.plc, st.i2s_running ? "on" : "off", st.out_rate, st.queued_blocks,
			st.drift_drop, st.drift_repeat, st.i2s_err + st.dec_err + st.q_full);
		log_link();
	}

	return 0;
}
