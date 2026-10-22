/*
 * Bike hub - che do Dinh vi (NAV), ban don gian: xe chi PHAN XA, khong tinh gi.
 * Thiet ke: docs/HELMET_NAV_DINH_VI.md
 *
 *   Bat / tat: Call ngan tren hub, hoac "bike nav on|off".
 *   Khi bat: phat quang ba Dinh vi (Service Data {UUID 0eed0201, ver, loai = xe, ma}) va bat vai
 *   tro CS REFLECTOR cho moi ket noi den. Mu (apps/main nav.c) thay quang ba thi ket noi toi
 *   xe, ma hoa roi TU tao cau hinh CS, bat CS Security, bat do va TU tinh khoang cach (mu =
 *   central = CS initiator, IPT). Xe khong co dich vu GATT nao cho Dinh vi.
 *
 *   Trong luc Dinh vi, xe TAM DUNG Auracast (BIG): BIG 10 ms, ca khi chi phat khung im lang, lam
 *   phan lon thu tuc CS mu-xe bi huy (do 22/10/2026: ~1 thu tuc thanh cong moi 10-30 s, trong khi
 *   mu-mu khong co BIG thi gan nhu thu tuc nao cung xong). "bike nav big keep" de giu BIG.
 *
 *   Xe nhan ra mot ket noi la mu Dinh vi khi mu tao cau hinh CS voi xe (su kien LE CS Config
 *   Complete o phia xe). Luc do goi hub_on_helmet() de bo ket noi khoi luong dien thoai (mu da
 *   ghep co the vao qua bo quang ba dien thoai: cung identity). Tat che do -> ngat cac mu nay.
 *
 * Luong: callback Bluetooth (bt_rx) chi ghi lai; lenh HCI dong bo (cai dat mac dinh CS, quang ba)
 * chay trong system workqueue.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "bike.h"
#include "nav_proto.h"

LOG_MODULE_REGISTER(nav_hub, LOG_LEVEL_INF);

#define MAX_HELMETS  CONFIG_BT_CTLR_SDC_CS_COUNT
#define ADV_INT_MIN  0x00A0 /* 100 ms */
#define ADV_INT_MAX  0x00F0 /* 150 ms */
#define ADV_CHECK_MS 2000   /* bat lai quang ba neu controller da dung (sau ket noi) */

struct helmet {
	struct bt_conn *conn; /* giu 1 ref */
	bool running;
	uint32_t n_ok;
	uint32_t n_abort;
	uint32_t aborted_pc;
};

static struct k_spinlock lock;
static struct helmet helmets[MAX_HELMETS];
static struct bt_conn *pending[CONFIG_BT_MAX_CONN]; /* cho bat vai tro phan xa (giu ref) */
static atomic_t enabled;
static atomic_t want_on;  /* lenh bat / tat moi nhat (nut, shell) */
static atomic_t keep_big; /* 1: khong tam dung Auracast khi Dinh vi */
static struct bt_le_ext_adv *adv_nav;
static uint8_t nav_svc[NAV_ADV_LEN];

static void setup_fn(struct k_work *w);
static void adv_fn(struct k_work *w);
static void mode_fn(struct k_work *w);
static K_WORK_DEFINE(setup_work, setup_fn);
static K_WORK_DELAYABLE_DEFINE(adv_work, adv_fn);
static K_WORK_DEFINE(mode_work, mode_fn);

static const struct bt_data ad_nav[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_SVC_DATA128, nav_svc, sizeof(nav_svc)),
};

/* ---------------- Tien ich ---------------- */

static struct helmet *helmet_of(const struct bt_conn *conn)
{
	for (int i = 0; i < MAX_HELMETS; i++) {
		if (helmets[i].conn != NULL && helmets[i].conn == conn) {
			return &helmets[i];
		}
	}
	return NULL;
}

static const char *addr_str(const struct bt_conn *conn, char *buf, size_t len)
{
	bt_addr_le_to_str(bt_conn_get_dst(conn), buf, len);
	return buf;
}

/* ---------------- Work (sysworkq) ---------------- */

/* Bat vai tro phan xa cho cac ket noi moi (lenh HCI dong bo: khong goi trong bt_rx) */
static void setup_fn(struct k_work *w)
{
	const struct bt_le_cs_set_default_settings_param p = {
		.enable_initiator_role = false,
		.enable_reflector_role = true,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};

	ARG_UNUSED(w);
	for (size_t i = 0; i < ARRAY_SIZE(pending); i++) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		struct bt_conn *c = pending[i];

		pending[i] = NULL;
		k_spin_unlock(&lock, key);
		if (c == NULL) {
			continue;
		}
		if (atomic_get(&enabled)) {
			const int err = bt_le_cs_set_default_settings(c, &p);

			if (err) {
				LOG_WRN("NAV: CS reflector setup failed (%d)", err);
			}
		}
		bt_conn_unref(c);
	}
}

/* Controller dung bo quang ba co the ket noi khi co ket noi: kiem tra dinh ky va bat lai.
 * Dang chay thi bt_le_ext_adv_start tra -EALREADY ngay (khong gui HCI).
 */
static void adv_fn(struct k_work *w)
{
	int err;

	ARG_UNUSED(w);
	if (adv_nav == NULL || !atomic_get(&enabled)) {
		return;
	}
	err = bt_le_ext_adv_start(adv_nav, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		LOG_DBG("NAV advertising start failed (%d) - retry", err);
	}
	k_work_reschedule(&adv_work, K_MSEC(ADV_CHECK_MS));
}

static void disconnect_helmets(void)
{
	for (int i = 0; i < MAX_HELMETS; i++) {
		struct bt_conn *c;
		k_spinlock_key_t key = k_spin_lock(&lock);

		c = helmets[i].conn != NULL ? bt_conn_ref(helmets[i].conn) : NULL;
		k_spin_unlock(&lock, key);
		if (c != NULL) {
			(void)bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			bt_conn_unref(c);
		}
	}
}

static void mode_fn(struct k_work *w)
{
	const bool on = atomic_get(&want_on) != 0;

	ARG_UNUSED(w);
	if (on == (atomic_get(&enabled) != 0) || adv_nav == NULL) {
		return;
	}
	atomic_set(&enabled, on);
	if (on) {
		LOG_INF("Navigation mode ON - advertising; helmets measure their distance to the bike");
		hub_tone(HUB_TONE_NAV_ON);
		if (!atomic_get(&keep_big)) {
			bcast_source_pause(true); /* nhuong radio cho Channel Sounding */
		}
		k_work_reschedule(&adv_work, K_NO_WAIT);
	} else {
		(void)k_work_cancel_delayable(&adv_work);
		(void)bt_le_ext_adv_stop(adv_nav);
		disconnect_helmets();
		bcast_source_pause(false);
		LOG_INF("Navigation mode OFF");
		hub_tone(HUB_TONE_NAV_OFF);
	}
}

/* ---------------- Callback Bluetooth (bt_rx) ---------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	k_spinlock_key_t key;

	if (err || !atomic_get(&enabled) || bt_conn_get_info(conn, &info) != 0 ||
	    info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;
	}
	/* Chua biet la mu hay dien thoai: bat phan xa cho moi ket noi den (vo hai voi dien thoai) */
	key = k_spin_lock(&lock);

	for (size_t i = 0; i < ARRAY_SIZE(pending); i++) {
		if (pending[i] == NULL) {
			pending[i] = bt_conn_ref(conn);
			break;
		}
	}
	k_spin_unlock(&lock, key);
	k_work_submit(&setup_work);
	k_work_reschedule(&adv_work, K_MSEC(100));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct bt_conn *drop = NULL;
	struct helmet h = {0};
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (int i = 0; i < MAX_HELMETS; i++) {
		if (helmets[i].conn != NULL && helmets[i].conn == conn) {
			h = helmets[i];
			drop = helmets[i].conn;
			memset(&helmets[i], 0, sizeof(helmets[i]));
			break;
		}
	}
	k_spin_unlock(&lock, key);
	if (drop == NULL) {
		return;
	}
	LOG_INF("NAV: helmet %s left (reason 0x%02x, %u procedures, %u aborted)",
		addr_str(conn, addr, sizeof(addr)), reason, h.n_ok, h.n_abort);
	bt_conn_unref(drop);
}

/* Mu vua tao cau hinh CS voi xe: day chinh la mu Dinh vi */
static void config_cb(struct bt_conn *conn, uint8_t status, struct bt_conn_le_cs_config *cfg)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bool added = false;
	k_spinlock_key_t key;

	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("NAV: CS config with %s failed (0x%02x)", addr_str(conn, addr, sizeof(addr)),
			status);
		return;
	}
	key = k_spin_lock(&lock);
	if (helmet_of(conn) == NULL) {
		for (int i = 0; i < MAX_HELMETS; i++) {
			if (helmets[i].conn == NULL) {
				memset(&helmets[i], 0, sizeof(helmets[i]));
				helmets[i].conn = bt_conn_ref(conn);
				helmets[i].aborted_pc = UINT32_MAX;
				added = true;
				break;
			}
		}
	}
	k_spin_unlock(&lock, key);
	if (!added) {
		return;
	}
	LOG_INF("NAV: helmet %s is measuring its distance to the bike (bike = CS reflector, mode %u)",
		addr_str(conn, addr, sizeof(addr)), cfg->mode);
	hub_on_helmet(conn); /* khong phai dien thoai */
}

static void proc_enable_cb(struct bt_conn *conn, uint8_t status,
			   struct bt_conn_le_cs_procedure_enable_complete *p)
{
	struct helmet *h = helmet_of(conn);
	char addr[BT_ADDR_LE_STR_LEN];

	if (h == NULL || status != BT_HCI_ERR_SUCCESS) {
		return;
	}
	h->running = p->state == BT_CONN_LE_CS_PROCEDURES_ENABLED;
	LOG_INF("NAV: helmet %s ranging %s (procedure every %u conn events)",
		addr_str(conn, addr, sizeof(addr)), h->running ? "RUNNING" : "stopped",
		p->procedure_interval);
}

static void subevent_cb(struct bt_conn *conn, struct bt_conn_le_cs_subevent_result *r)
{
	struct helmet *h = helmet_of(conn);

	if (h == NULL) {
		return;
	}
	if (r->header.procedure_abort_reason != BT_HCI_LE_CS_PROCEDURE_ABORT_REASON_NO_ABORT ||
	    r->header.subevent_abort_reason != BT_HCI_LE_CS_SUBEVENT_ABORT_REASON_NO_ABORT) {
		if (h->aborted_pc != r->header.procedure_counter) {
			h->aborted_pc = r->header.procedure_counter;
			h->n_abort++;
		}
		return;
	}
	if (r->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
		h->n_ok++;
	}
}

BT_CONN_CB_DEFINE(nav_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_cs_config_complete = config_cb,
	.le_cs_procedure_enable_complete = proc_enable_cb,
	.le_cs_subevent_data_available = subevent_cb,
};

/* ---------------- API ---------------- */

int nav_hub_init(void)
{
	static const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_INT_MIN, ADV_INT_MAX, NULL);
	bt_addr_le_t ids[CONFIG_BT_ID_MAX];
	size_t n = ARRAY_SIZE(ids);
	int err;

	bt_id_get(ids, &n);
	nav_adv_build(nav_svc, NAV_TYPE_BIKE, n > 0 ? nav_tag_of(&ids[0]) : 0);
	/* Legacy, co the ket noi: 27 B, it chiem radio hon extended */
	err = bt_le_ext_adv_create(&param, NULL, &adv_nav);
	if (err) {
		LOG_ERR("NAV advertising set create failed (%d)", err);
		return err;
	}
	err = bt_le_ext_adv_set_data(adv_nav, ad_nav, ARRAY_SIZE(ad_nav), NULL, 0);
	if (err) {
		LOG_ERR("NAV advertising data failed (%d)", err);
		return err;
	}
	LOG_INF("Navigation mode ready (Call short or \"bike nav on\")");
	return 0;
}

void nav_hub_set(bool on)
{
	atomic_set(&want_on, on);
	k_work_submit(&mode_work);
}

void nav_hub_toggle(void)
{
	nav_hub_set(!atomic_get(&want_on));
}

void nav_hub_log_status(void)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!atomic_get(&enabled)) {
		return;
	}
	for (int i = 0; i < MAX_HELMETS; i++) {
		struct bt_conn *c;
		struct helmet h;
		k_spinlock_key_t key = k_spin_lock(&lock);

		c = helmets[i].conn != NULL ? bt_conn_ref(helmets[i].conn) : NULL;
		h = helmets[i];
		k_spin_unlock(&lock, key);
		if (c == NULL) {
			continue;
		}
		LOG_INF("nav: helmet %s %s, %u procedures, %u aborted", addr_str(c, addr, sizeof(addr)),
			h.running ? "RUNNING" : "setting up", h.n_ok, h.n_abort);
		bt_conn_unref(c);
	}
}

int nav_hub_shell(const struct shell *sh, size_t argc, char **argv)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int n = 0;

	if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
		nav_hub_set(strcmp(argv[1], "on") == 0);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "big") == 0 &&
	    (strcmp(argv[2], "pause") == 0 || strcmp(argv[2], "keep") == 0)) {
		atomic_set(&keep_big, strcmp(argv[2], "keep") == 0);
		shell_print(sh, "Auracast during navigation: %s (from the next time navigation is "
			    "turned on)",
			    atomic_get(&keep_big) ? "kept" : "paused");
		return 0;
	}
	if (argc != 1) {
		shell_error(sh, "usage: bike nav [on|off|big <pause|keep>]");
		return -EINVAL;
	}
	shell_print(sh, "Nav       : %s, Auracast %s while navigating",
		    atomic_get(&enabled) ? "ON (advertising, CS reflector)"
					 : "off (Call short or bike nav on)",
		    atomic_get(&keep_big) ? "kept" : "paused");
	for (int i = 0; i < MAX_HELMETS; i++) {
		struct bt_conn *c;
		struct helmet h;
		k_spinlock_key_t key = k_spin_lock(&lock);

		c = helmets[i].conn != NULL ? bt_conn_ref(helmets[i].conn) : NULL;
		h = helmets[i];
		k_spin_unlock(&lock, key);
		if (c == NULL) {
			continue;
		}
		n++;
		shell_print(sh, "Helmet    : %s, %s, %u procedures, %u aborted",
			    addr_str(c, addr, sizeof(addr)), h.running ? "RUNNING" : "setting up",
			    h.n_ok, h.n_abort);
		bt_conn_unref(c);
	}
	if (n == 0) {
		shell_print(sh, "Helmet    : none (distances are computed and logged on the helmets)");
	}
	return 0;
}
