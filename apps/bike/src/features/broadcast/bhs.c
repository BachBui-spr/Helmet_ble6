/*
 * Bike hub - Bike Hub Service (BHS) va cua so ghep mu.
 *
 * Ghep bang nut (khong go ma):
 *   Hub:  Power ngan -> giu Vol+ 2 s  -> mo cua so 60 s: bo quang ba rieng, co the ket noi, mang
 *         Service Data 128 bit {BHS UUID, ver, co "dang ghep"} (am "dang tim" lap).
 *   Mu:   Power ngan -> giu Call 2 s  -> mu quet, thay co "dang ghep" -> ket noi (central) -> ma
 *         hoa (Just Works, luu bond) -> doc Join Info -> ngat -> vao broadcast ngay, luu lai de
 *         nhung lan bat sau tu vao (apps/main/src/features/broadcast/hub_link.c + broadcast_sink.c).
 *   Join Info chi doc duoc khi lien ket da ma hoa VA cua so ghep dang mo, nen ma broadcast chi
 *   den tay mu ma nguoi dung chu dong ghep.
 *
 * Nhan dien mu: ket noi nao doc Join Info la mu (hub_on_helmet); dien thoai khong doc dac tinh
 * nay. Mu tu ngat sau khi doc; hub ngat ho sau 3 s neu mu chua ngat.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "app/app.h"
#include "features/broadcast/bcast_source.h"
#include "features/broadcast/bhs.h"

LOG_MODULE_REGISTER(bhs, LOG_LEVEL_INF);

#define PAIR_WINDOW_S  60
#define HELMET_LINK_MS 3000 /* sau khi doc Join Info: mu chua tu ngat thi hub ngat */
#define MAX_HELMET_LINKS (CONFIG_BT_MAX_CONN)

static const struct bt_uuid_128 bhs_uuid = BT_UUID_INIT_128(BHS_UUID_SVC_VAL);
static const struct bt_uuid_128 join_uuid = BT_UUID_INIT_128(BHS_UUID_JOIN_VAL);

static atomic_t window_open;
static atomic_t joined_window; /* so mu doc Join Info trong cua so hien tai */
static uint32_t joined_total;

/* Ket noi da doc Join Info (bt_rx ghi, work doc) */
static struct k_spinlock lock;
static struct bt_conn *helmets[MAX_HELMET_LINKS];
static struct bt_conn *pending_helmet; /* vua doc, cho work xu ly */

static struct bt_le_ext_adv *adv_pair;

static void close_work_fn(struct k_work *w);
static void helmet_work_fn(struct k_work *w);
static void adv_work_fn(struct k_work *w);
static void kick_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(close_work, close_work_fn);
static K_WORK_DEFINE(helmet_work, helmet_work_fn);
static K_WORK_DEFINE(pair_adv_work, adv_work_fn);
static K_WORK_DELAYABLE_DEFINE(kick_work, kick_work_fn);

/* ---------------- GATT ---------------- */

static bool is_helmet_locked(const struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(helmets); i++) {
		if (helmets[i] == conn) {
			return true;
		}
	}
	return false;
}

static ssize_t join_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	uint8_t v[BHS_JOIN_INFO_LEN];
	k_spinlock_key_t key;
	bool fresh = false;

	if (!atomic_get(&window_open)) {
		/* Chi trong cua so ghep (nguoi dung vua bam nut tren hub) */
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	bcast_source_join_info(v);

	if (offset == 0) {
		key = k_spin_lock(&lock);
		if (!is_helmet_locked(conn)) {
			for (size_t i = 0; i < ARRAY_SIZE(helmets); i++) {
				if (helmets[i] == NULL) {
					helmets[i] = bt_conn_ref(conn);
					pending_helmet = helmets[i];
					fresh = true;
					break;
				}
			}
		}
		k_spin_unlock(&lock, key);
		if (fresh) {
			k_work_submit(&helmet_work);
		}
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(v));
}

/* Dich vu tinh: co trong co so du lieu GATT ngay tu luc build (Database Hash on dinh) */
BT_GATT_SERVICE_DEFINE(bhs_svc,
	BT_GATT_PRIMARY_SERVICE(&bhs_uuid),
	BT_GATT_CHARACTERISTIC(&join_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
			       join_read, NULL, NULL),
);

/* ---------------- Quang ba cua so ghep ---------------- */

static const uint8_t pair_svc[] = {BHS_UUID_SVC_VAL, BHS_PROTO_VER, BHS_FLAG_PAIRING};

/* KHONG co ten va KHONG co co "general discoverable": mu nhan ra hub bang Service Data. Truoc day
 * co ten "Bike_Hub" -> trong 60 s cua so ghep, dien thoai / may tinh bang thay HAI "Bike_Hub" (moi
 * bo quang ba mot dia chi RPA rieng) va co the ket noi nham vao bo quang ba nay (do 23/10/2026).
 */
static const struct bt_data ad_pair[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_SVC_DATA128, pair_svc, sizeof(pair_svc)),
};

/* Controller tu dung bo quang ba co the ket noi khi co ket noi den: bat lai cho mu ke tiep */
static void adv_work_fn(struct k_work *w)
{
	int err;

	ARG_UNUSED(w);
	if (!atomic_get(&window_open) || adv_pair == NULL) {
		return;
	}
	err = bt_le_ext_adv_start(adv_pair, BT_LE_EXT_ADV_START_DEFAULT);
	if (err && err != -EALREADY) {
		LOG_WRN("Pairing advertising start failed (%d) - retry", err);
		k_work_reschedule(&kick_work, K_MSEC(500)); /* het o ket noi: thu lai */
	}
}

static void adv_connected(struct bt_le_ext_adv *adv, struct bt_le_ext_adv_connected_info *info)
{
	ARG_UNUSED(adv);
	ARG_UNUSED(info);
	k_work_submit(&pair_adv_work);
}

static struct bt_le_ext_adv_cb pair_adv_cb = {
	.connected = adv_connected,
};

/* ---------------- Mu da doc Join Info ---------------- */

static void helmet_work_fn(struct k_work *w)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bt_conn *conn = pending_helmet;
	char addr[BT_ADDR_LE_STR_LEN];

	ARG_UNUSED(w);
	pending_helmet = NULL;
	k_spin_unlock(&lock, key);
	if (conn == NULL) {
		return;
	}

	atomic_inc(&joined_window);
	joined_total++;
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Helmet %s joined (%u in this window)", addr, (unsigned)atomic_get(&joined_window));
	hub_on_helmet(conn);
	hub_tone(HUB_TONE_HELMET_JOINED);
	/* Am "dang tim" lap lai sau am bao vao */
	k_work_reschedule(&kick_work, K_MSEC(HELMET_LINK_MS));
}

/* Ngat ho mu da doc xong ma chua tu ngat; bat lai am / quang ba cua so */
static void kick_work_fn(struct k_work *w)
{
	struct bt_conn *list[MAX_HELMET_LINKS];
	k_spinlock_key_t key;
	size_t n = 0;

	ARG_UNUSED(w);
	key = k_spin_lock(&lock);
	for (size_t i = 0; i < ARRAY_SIZE(helmets); i++) {
		if (helmets[i] != NULL) {
			list[n++] = bt_conn_ref(helmets[i]);
		}
	}
	k_spin_unlock(&lock, key);
	for (size_t i = 0; i < n; i++) {
		(void)bt_conn_disconnect(list[i], BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(list[i]);
	}
	if (atomic_get(&window_open)) {
		hub_tone(HUB_TONE_PAIR_SEARCH);
		k_work_submit(&pair_adv_work);
	}
}

void bhs_conn_disconnected(struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bt_conn *drop = NULL;

	for (size_t i = 0; i < ARRAY_SIZE(helmets); i++) {
		if (helmets[i] == conn) {
			drop = helmets[i];
			helmets[i] = NULL;
		}
	}
	if (pending_helmet == drop) {
		pending_helmet = NULL;
	}
	k_spin_unlock(&lock, key);
	if (drop != NULL) {
		bt_conn_unref(drop);
	}
	if (atomic_get(&window_open)) {
		k_work_submit(&pair_adv_work);
	}
}

bool bhs_is_helmet(const struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const bool r = is_helmet_locked(conn);

	k_spin_unlock(&lock, key);
	return r;
}

/* ---------------- Cua so ghep ---------------- */

static void close_work_fn(struct k_work *w)
{
	const atomic_val_t n = atomic_get(&joined_window);

	ARG_UNUSED(w);
	if (!atomic_cas(&window_open, 1, 0)) {
		return;
	}
	if (adv_pair != NULL) {
		(void)bt_le_ext_adv_stop(adv_pair);
	}
	LOG_INF("Helmet pairing window closed: %d helmet(s) joined", (int)n);
	hub_tone(n > 0 ? HUB_TONE_PAIR_END : HUB_TONE_FAIL);
}

void bhs_pair_start(void)
{
	if (atomic_cas(&window_open, 0, 1)) {
		atomic_set(&joined_window, 0);
		LOG_INF("Helmet pairing window OPEN for %d s - on each helmet: Power short, then hold "
			"Call 2 s",
			PAIR_WINDOW_S);
	} else {
		LOG_INF("Helmet pairing window extended to %d s", PAIR_WINDOW_S);
	}
	hub_tone(HUB_TONE_PAIR_SEARCH);
	k_work_submit(&pair_adv_work);
	k_work_reschedule(&close_work, K_SECONDS(PAIR_WINDOW_S));
}

void bhs_pair_cancel(void)
{
	if (atomic_get(&window_open)) {
		LOG_INF("Helmet pairing window cancelled");
		k_work_reschedule(&close_work, K_NO_WAIT);
	}
}

bool bhs_pair_active(void)
{
	return atomic_get(&window_open) != 0;
}

int bhs_init(void)
{
	static const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_EXT_ADV, BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	int err;

	err = bt_le_ext_adv_create(&param, &pair_adv_cb, &adv_pair);
	if (err) {
		LOG_ERR("Pairing advertising set create failed (%d)", err);
		return err;
	}
	err = bt_le_ext_adv_set_data(adv_pair, ad_pair, ARRAY_SIZE(ad_pair), NULL, 0);
	if (err) {
		LOG_ERR("Pairing advertising data failed (%d)", err);
	}
	return err;
}

void bhs_print(const struct shell *sh)
{
	shell_print(sh, "Pairing   : window %s, helmets joined %u total (%d this window)",
		    atomic_get(&window_open) ? "OPEN" : "closed", joined_total,
		    (int)atomic_get(&joined_window));
}
