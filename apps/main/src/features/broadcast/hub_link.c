/*
 * Helmet - lien ket tam toi Bike hub (apps/bike) de lay "thong tin vao xe" (Join Info).
 * Thiet ke: docs/BIKE_HUB_THIET_KE.md muc 8
 *
 * broadcast_sink.c thay hub dang mo cua so ghep (Service Data BHS co co "dang ghep") trong luc tu
 * tim -> hub_link_start(): ket noi (central) -> ma hoa (Just Works, luu bond) -> tim dac tinh
 * Join Info -> doc (doc dai neu can) -> ngat -> bcast_hub_result() voi broadcast ID, dia chi +
 * SID bo phat broadcast va ma broadcast. Khong ai phai go ma.
 * Moi callback chay trong bt_rx; app/app_bt.c chuyen cac su kien ket noi cua lien ket nay vao day.
 *
 * Khoa ghep doi cu lech nhau (mot ben da xoa ghep doi): ma hoa that bai voi "key missing" /
 * "key rejected" / "auth fail", HOAC mu da co ghep doi voi hub nay ma ma hoa hong vi bat ky ly do
 * gi (ke ca lien ket bi ngat giua chung) -> bao -EAGAIN kem dia chi hub, sau khi lien ket ngat
 * han; broadcast_sink.c xoa khoa cu voi dung hub do roi tu quet + ghep lai mot lan (cua so ghep
 * tren hub van mo). Khong phu thuoc hub tra ma loi nao.
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
#include <zephyr/sys/atomic.h>

#include "features/broadcast/broadcast.h"

LOG_MODULE_REGISTER(hub_link, LOG_LEVEL_INF);

#define HUB_LINK_TIMEOUT_S 15
/* Chu ky ket noi 11,25 ms (9 x 1,25 ms): KHONG la boi so cua chu ky ISO 10 ms cua BIG tren hub.
 * Chu ky 30-50 ms (mac dinh) la boi so -> moi su kien roi cung mot pha trong chu ky BIG; trung
 * luc hub dang phat BIS thi bi bo gan nhu lien tuc (MTU 12 s, ghep het gio). Lech 1,25 ms moi su
 * kien -> quet het chu ky BIG sau 8 su kien (90 ms), khoang mot nua roi vao khoang trong cua hub.
 * Lien ket chi song ~1 s nen chu ky ngan khong ton pin.
 */
#define HUB_LINK_INT       9   /* x 1,25 ms */
#define HUB_LINK_SUP_TO    400 /* x 10 ms = 4 s */

static const struct bt_uuid_128 join_uuid = BT_UUID_INIT_128(BHS_UUID_JOIN_VAL);

static struct bt_conn *hub_conn; /* giu ref tu bt_conn_le_create toi khi ngat */
static atomic_t busy;
static atomic_t done; /* da bao ket qua cho broadcast_sink */
static struct bt_gatt_discover_params disc;
static struct bt_gatt_read_params rd;
static uint8_t info[BHS_JOIN_INFO_LEN];
static uint16_t info_len;
static bt_addr_le_t stale_peer; /* hub co khoa ghep doi cu khong khop */
static bool stale_valid;        /* stale_peer hop le (hub_link_stale_peer lay ra) */
static bool stale_pending;      /* bao -EAGAIN khi lien ket da ngat han */
static bool had_bond;           /* mu da co ghep doi voi hub nay truoc lan ket noi nay */
static bool secured;            /* ma hoa da xong */

static void timeout_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_fn);

static void finish(int err)
{
	if (atomic_set(&done, 1)) {
		return;
	}
	(void)k_work_cancel_delayable(&timeout_work);
	if (err) {
		LOG_WRN("Hub link failed (%d)", err);
	}
	bcast_hub_result(err, info, info_len);
	if (hub_conn != NULL) {
		(void)bt_conn_disconnect(hub_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void timeout_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	finish(-ETIMEDOUT);
}

struct bond_query {
	const bt_addr_le_t *addr;
	bool found;
};

static void bond_cb(const struct bt_bond_info *bi, void *user_data)
{
	struct bond_query *q = user_data;

	if (bt_addr_le_eq(&bi->addr, q->addr)) {
		q->found = true;
	}
}

static bool bond_exists(const bt_addr_le_t *addr)
{
	struct bond_query q = {.addr = addr};

	bt_foreach_bond(BT_ID_DEFAULT, bond_cb, &q);
	return q.found;
}

/* Khoa cu: ghi dia chi hub, ngat; ket qua -EAGAIN bao trong hub_link_disconnected() */
static void mark_stale(struct bt_conn *conn)
{
	bt_addr_le_copy(&stale_peer, bt_conn_get_dst(conn));
	stale_valid = true;
	stale_pending = true;
	atomic_set(&done, 1);
	(void)k_work_cancel_delayable(&timeout_work);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static uint8_t read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t length)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err) {
		/* 0x08 Insufficient Authorization: hub da dong cua so ghep */
		LOG_WRN("Join Info read error 0x%02x", err);
		finish(-EACCES);
		return BT_GATT_ITER_STOP;
	}
	if (data == NULL) {
		finish(info_len == BHS_JOIN_INFO_LEN ? 0 : -EBADMSG);
		return BT_GATT_ITER_STOP;
	}
	if (info_len + length > sizeof(info)) {
		finish(-EMSGSIZE);
		return BT_GATT_ITER_STOP;
	}
	memcpy(&info[info_len], data, length);
	info_len += length;
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t disc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		       struct bt_gatt_discover_params *params)
{
	const struct bt_gatt_chrc *chrc;
	int err;

	ARG_UNUSED(params);
	if (attr == NULL) {
		LOG_WRN("Bike Hub Service not found on peer");
		finish(-ENOENT);
		return BT_GATT_ITER_STOP;
	}

	chrc = attr->user_data;
	memset(&rd, 0, sizeof(rd));
	rd.func = read_cb;
	rd.handle_count = 1;
	rd.single.handle = chrc->value_handle;
	rd.single.offset = 0;
	info_len = 0;
	err = bt_gatt_read(conn, &rd);
	if (err) {
		finish(err);
	}
	return BT_GATT_ITER_STOP;
}

bool hub_link_stale_peer(bt_addr_le_t *out)
{
	if (!stale_valid) {
		return false;
	}
	bt_addr_le_copy(out, &stale_peer);
	stale_valid = false;
	return true;
}

int hub_link_start(const bt_addr_le_t *addr)
{
	char str[BT_ADDR_LE_STR_LEN];
	int err;

	if (atomic_set(&busy, 1)) {
		return -EBUSY;
	}
	atomic_set(&done, 0);
	info_len = 0;
	stale_valid = false;
	stale_pending = false;
	had_bond = false;
	secured = false;

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM(HUB_LINK_INT, HUB_LINK_INT, 0, HUB_LINK_SUP_TO), &hub_conn);
	if (err) {
		atomic_set(&busy, 0);
		hub_conn = NULL;
		return err;
	}
	bt_addr_le_to_str(addr, str, sizeof(str));
	LOG_INF("Connecting to bike hub %s", str);
	k_work_reschedule(&timeout_work, K_SECONDS(HUB_LINK_TIMEOUT_S));
	return 0;
}

void hub_link_cancel(void)
{
	if (atomic_get(&busy) && !atomic_get(&done)) {
		atomic_set(&done, 1); /* khong bao ket qua nua */
		(void)k_work_cancel_delayable(&timeout_work);
		if (hub_conn != NULL) {
			(void)bt_conn_disconnect(hub_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
}

bool hub_link_owned(const struct bt_conn *conn)
{
	return conn != NULL && conn == hub_conn;
}

void hub_link_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info ci;

	if (err) {
		LOG_WRN("Hub connection failed (0x%02x %s)", err, bt_hci_err_to_str(err));
		finish(-ECONNREFUSED);
		/* Ket noi chua thanh lap: khong co disconnected() -> giai phong o day */
		bt_conn_unref(hub_conn);
		hub_conn = NULL;
		atomic_set(&busy, 0);
		return;
	}
	had_bond = bond_exists(bt_conn_get_dst(conn));
	if (bt_conn_get_info(conn, &ci) == 0) {
		LOG_INF("Hub link up: interval %u.%02u ms, timeout %u ms, %s",
			ci.le.interval_us / 1000, (ci.le.interval_us % 1000) / 10,
			ci.le.timeout * 10, had_bond ? "bonded before" : "new pairing");
	}
	/* Join Info yeu cau ma hoa: ghep doi Just Works (luu bond) */
	if (bt_conn_set_security(conn, BT_SECURITY_L2) != 0) {
		finish(-EACCES);
	}
}

void hub_link_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	int ret;

	if (err) {
		LOG_WRN("Hub link security failed (%d %s)", err, bt_security_err_to_str(err));
		if (had_bond || err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING ||
		    err == BT_SECURITY_ERR_KEY_REJECTED || err == BT_SECURITY_ERR_AUTH_FAIL) {
			/* Khoa cu: hub da xoa ghep doi (hoac nguoc lai) -> xoa va ghep lai. Bao sau
			 * khi ngat han de lan ket noi lai khong gap lien ket cu (busy / -EBUSY).
			 */
			mark_stale(conn);
		} else {
			finish(-EACCES);
		}
		return;
	}
	secured = true;
	LOG_INF("Hub link encrypted (level %d) - reading Join Info", level);
	memset(&disc, 0, sizeof(disc));
	disc.uuid = &join_uuid.uuid;
	disc.func = disc_cb;
	disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	ret = bt_gatt_discover(conn, &disc);
	if (ret) {
		finish(ret);
	}
}

void hub_link_disconnected(struct bt_conn *conn, uint8_t reason)
{
	const bool stale = stale_pending;

	LOG_INF("Hub link closed (reason 0x%02x %s)", reason, bt_hci_err_to_str(reason));
	if (!atomic_get(&done) && had_bond && !secured) {
		/* Ngat giua luc ma hoa bang khoa cu (vd hub ngat 0x05): cung coi la khoa cu */
		mark_stale(conn);
	}
	finish(-ENOTCONN); /* bo qua neu da bao ket qua */
	bt_conn_unref(hub_conn);
	hub_conn = NULL;
	stale_pending = false;
	atomic_set(&busy, 0);
	if (stale) {
		bcast_hub_result(-EAGAIN, NULL, 0);
	}
}
