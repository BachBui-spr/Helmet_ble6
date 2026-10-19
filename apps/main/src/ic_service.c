/*
 * Helmet - Helmet Intercom Service (HIS): kenh dieu khien giua hai mu.
 * Thiet ke: docs/HELMET_THIET_KE_INTERCOM.md muc 6.2
 *
 *   Control Point (Write, can ma hoa): master -> slave
 *   Event         (Indicate, can ma hoa): slave -> master
 *   Tin nhan 4 byte: [op][session][arg][arg2] (enum ic_op trong helmet.h)
 *
 * Dich vu tinh (BT_GATT_SERVICE_DEFINE): co san trong co so du lieu GATT truoc settings_load(),
 * khong lam lech Database Hash giua cac lan khoi dong.
 * Moi callback chay trong bt_rx: chi chuyen tiep cho intercom.c (intercom_on_*).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "helmet.h"

LOG_MODULE_REGISTER(ic_svc, LOG_LEVEL_INF);

static const struct bt_uuid_128 his_uuid = BT_UUID_INIT_128(IC_UUID_SVC_VAL);
static const struct bt_uuid_128 cp_uuid = BT_UUID_INIT_128(IC_UUID_CP_VAL);
static const struct bt_uuid_128 evt_uuid = BT_UUID_INIT_128(IC_UUID_EVT_VAL);

/* ---------------- Server (moi mu; dung khi la slave) ---------------- */

static ssize_t cp_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != IC_MSG_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	intercom_on_msg(conn, p[0], p[2]);
	return len;
}

static void evt_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_DBG("Event CCC 0x%04x", value);
}

/* attrs: [0] dich vu, [1] khai bao CP, [2] gia tri CP, [3] khai bao Event, [4] gia tri Event,
 * [5] CCC cua Event
 */
BT_GATT_SERVICE_DEFINE(his_svc,
	BT_GATT_PRIMARY_SERVICE(&his_uuid),
	BT_GATT_CHARACTERISTIC(&cp_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT, NULL,
			       cp_write, NULL),
	BT_GATT_CHARACTERISTIC(&evt_uuid.uuid, BT_GATT_CHRC_INDICATE, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(evt_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
);

#define EVT_ATTR (&his_svc.attrs[4])

static struct bt_gatt_indicate_params ind_params;
static uint8_t ind_buf[IC_MSG_LEN];
static atomic_t ind_busy;

static void ind_done(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);
	if (err) {
		LOG_WRN("Event indication failed (0x%02x)", err);
	}
	atomic_clear(&ind_busy);
}

/* ---------------- Client (dung khi la master) ---------------- */

static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_discover_params ccc_disc_params;
static struct bt_gatt_subscribe_params sub_params;
static struct bt_gatt_write_params wr_params;
static uint8_t wr_buf[IC_MSG_LEN];
static atomic_t wr_busy;
static uint16_t svc_end;
static uint16_t cp_handle;
static uint16_t evt_handle;

static uint8_t evt_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			  const void *data, uint16_t length)
{
	const uint8_t *p = data;

	ARG_UNUSED(params);

	if (data == NULL) {
		return BT_GATT_ITER_STOP; /* da huy dang ky (vd mat ket noi) */
	}
	if (length == IC_MSG_LEN) {
		intercom_on_msg(conn, p[0], p[2]);
	}
	return BT_GATT_ITER_CONTINUE;
}

static void sub_done(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(params);
	intercom_on_discovered(conn, err ? -EIO : 0);
}

static void subscribe_evt(struct bt_conn *conn)
{
	int err;

	memset(&sub_params, 0, sizeof(sub_params));
	sub_params.notify = evt_notify;
	sub_params.subscribe = sub_done;
	sub_params.value = BT_GATT_CCC_INDICATE;
	sub_params.value_handle = evt_handle;
	sub_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
	sub_params.end_handle = svc_end;
	sub_params.disc_params = &ccc_disc_params;
	/* Dang ky lai tu dau moi lan ket noi, khong de Zephyr giu qua lan ngat */
	atomic_set_bit(sub_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

	err = bt_gatt_subscribe(conn, &sub_params);
	if (err) {
		LOG_WRN("Subscribe to Event failed (%d)", err);
		intercom_on_discovered(conn, err);
	}
}

static uint8_t disc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		       struct bt_gatt_discover_params *params)
{
	int err;

	if (attr == NULL) {
		if (params->type == BT_GATT_DISCOVER_PRIMARY) {
			LOG_WRN("HIS not found on peer");
			intercom_on_discovered(conn, -ENOENT);
		} else if (cp_handle && evt_handle) {
			subscribe_evt(conn);
		} else {
			LOG_WRN("HIS characteristics missing (CP 0x%04x, Event 0x%04x)", cp_handle,
				evt_handle);
			intercom_on_discovered(conn, -ENOENT);
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *svc = attr->user_data;

		svc_end = svc->end_handle;
		params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
		params->uuid = NULL;
		params->start_handle = attr->handle + 1;
		params->end_handle = svc_end;
		err = bt_gatt_discover(conn, params);
		if (err) {
			intercom_on_discovered(conn, err);
		}
		return BT_GATT_ITER_STOP;
	}

	{
		const struct bt_gatt_chrc *chrc = attr->user_data;

		if (bt_uuid_cmp(chrc->uuid, &cp_uuid.uuid) == 0) {
			cp_handle = chrc->value_handle;
		} else if (bt_uuid_cmp(chrc->uuid, &evt_uuid.uuid) == 0) {
			evt_handle = chrc->value_handle;
		}
	}
	return BT_GATT_ITER_CONTINUE;
}

int ic_svc_discover(struct bt_conn *conn)
{
	cp_handle = 0;
	evt_handle = 0;
	svc_end = 0;
	atomic_clear(&wr_busy);

	memset(&disc_params, 0, sizeof(disc_params));
	disc_params.uuid = &his_uuid.uuid;
	disc_params.func = disc_cb;
	disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc_params.type = BT_GATT_DISCOVER_PRIMARY;
	return bt_gatt_discover(conn, &disc_params);
}

static void wr_done(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);
	if (err) {
		LOG_WRN("Control Point write failed (0x%02x)", err);
	}
	atomic_clear(&wr_busy);
}

/* ---------------- Gui tin nhan ---------------- */

int ic_svc_send(struct bt_conn *conn, bool to_slave, uint8_t op, uint8_t arg)
{
	const uint8_t msg[IC_MSG_LEN] = {op, 0, arg, 0};
	int err;

	if (to_slave) {
		/* Minh la master: ghi Control Point cua slave */
		if (cp_handle == 0) {
			return -ENOENT;
		}
		if (atomic_set(&wr_busy, 1)) {
			return -EBUSY;
		}
		memcpy(wr_buf, msg, sizeof(msg));
		memset(&wr_params, 0, sizeof(wr_params));
		wr_params.func = wr_done;
		wr_params.handle = cp_handle;
		wr_params.data = wr_buf;
		wr_params.length = sizeof(wr_buf);
		err = bt_gatt_write(conn, &wr_params);
		if (err) {
			atomic_clear(&wr_busy);
		}
		return err;
	}

	/* Minh la slave: indicate Event cho master */
	if (!bt_gatt_is_subscribed(conn, EVT_ATTR, BT_GATT_CCC_INDICATE)) {
		return -EAGAIN;
	}
	if (atomic_set(&ind_busy, 1)) {
		return -EBUSY;
	}
	memcpy(ind_buf, msg, sizeof(msg));
	memset(&ind_params, 0, sizeof(ind_params));
	ind_params.attr = EVT_ATTR;
	ind_params.func = ind_done;
	ind_params.data = ind_buf;
	ind_params.len = sizeof(ind_buf);
	err = bt_gatt_indicate(conn, &ind_params);
	if (err) {
		atomic_clear(&ind_busy);
	}
	return err;
}

bool ic_svc_busy(void)
{
	return atomic_get(&wr_busy) || atomic_get(&ind_busy);
}
