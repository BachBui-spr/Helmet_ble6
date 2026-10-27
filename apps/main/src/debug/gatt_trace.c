/*
 * Helmet - CHAN DOAN (tam thoi): ghi log moi lan thiet bi kia doc / ghi mot thuoc tinh GATT
 * cua mu. Dung de xem dien thoai lam gi voi PACS / BASS / ASCS khi "chia se am thanh".
 *
 * CONFIG_BT_GATT_AUTHORIZATION_CUSTOM: Zephyr hoi ung dung truoc moi lenh ATT Read / Write
 * (att.c, sau khi da kiem tra quyen + ma hoa). Module nay luon cho phep, chi ghi log.
 * Bo qua khai bao dich vu / dac tinh (doc khi do tim). Moi lenh GHI deu duoc ghi log; lenh DOC
 * lap lai cung handle trong 200 ms (Read Blob cua mot gia tri dai) chi ghi mot lan.
 * Bat / tat: "helmet trace [on|off]" (mac dinh bat).
 * Callback chay trong bt_rx.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "debug/gatt_trace.h"

LOG_MODULE_REGISTER(gatt_trace, LOG_LEVEL_INF);

struct uuid_name {
	uint16_t uuid;
	const char *name;
};

static const struct uuid_name names[] = {
	{BT_UUID_GAP_DEVICE_NAME_VAL, "GAP Device Name"},
	{BT_UUID_GAP_APPEARANCE_VAL, "GAP Appearance"},
	{BT_UUID_GAP_PPCP_VAL, "GAP Preferred Conn Params"},
	{BT_UUID_CENTRAL_ADDR_RES_VAL, "GAP Central Address Resolution"},
	{BT_UUID_GATT_SC_VAL, "GATT Service Changed"},
	{BT_UUID_GATT_CLIENT_FEATURES_VAL, "GATT Client Supported Features"},
	{BT_UUID_GATT_DB_HASH_VAL, "GATT Database Hash"},
	{BT_UUID_GATT_SERVER_FEATURES_VAL, "GATT Server Supported Features"},
	{BT_UUID_BASS_CONTROL_POINT_VAL, "BASS Control Point"},
	{BT_UUID_BASS_RECV_STATE_VAL, "BASS Receive State"},
	{BT_UUID_PACS_SNK_VAL, "PACS Sink PAC"},
	{BT_UUID_PACS_SNK_LOC_VAL, "PACS Sink Audio Locations"},
	{BT_UUID_PACS_SRC_VAL, "PACS Source PAC"},
	{BT_UUID_PACS_SRC_LOC_VAL, "PACS Source Audio Locations"},
	{BT_UUID_PACS_AVAILABLE_CONTEXT_VAL, "PACS Available Contexts"},
	{BT_UUID_PACS_SUPPORTED_CONTEXT_VAL, "PACS Supported Contexts"},
	{BT_UUID_ASCS_ASE_SNK_VAL, "ASCS Sink ASE"},
	{BT_UUID_ASCS_ASE_SRC_VAL, "ASCS Source ASE"},
	{BT_UUID_ASCS_ASE_CP_VAL, "ASCS ASE Control Point"},
	{BT_UUID_VCS_STATE_VAL, "VCS Volume State"},
	{BT_UUID_VCS_CONTROL_VAL, "VCS Control Point"},
	{BT_UUID_VCS_FLAGS_VAL, "VCS Volume Flags"},
	{BT_UUID_MICS_MUTE_VAL, "MICS Mute"},
	{BT_UUID_GATT_TMAPR_VAL, "TMAS Role"},
	{BT_UUID_HTS_MEASUREMENT_VAL, "HTS Temperature"},
	{BT_UUID_HTS_TEMP_TYP_VAL, "HTS Temperature Type"},
	{BT_UUID_HTS_TEMP_INT_VAL, "HTS Intermediate Temperature"},
	{BT_UUID_HTS_INTERVAL_VAL, "HTS Measurement Interval"},
	{BT_UUID_GATT_CUD_VAL, "User Description"},
};

static atomic_t enabled = ATOMIC_INIT(1);

/* Chi bt_rx dung: bo qua Read Blob lien tiep cua cung mot thuoc tinh */
#define READ_DEDUP_MS 200
static uint16_t last_handle;
static uint8_t last_conn = 0xff;
static int64_t last_read_ms;

static const char *uuid_name(const struct bt_uuid *uuid, char *buf, size_t len)
{
	if (uuid->type == BT_UUID_TYPE_16) {
		const uint16_t v = BT_UUID_16(uuid)->val;

		for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
			if (names[i].uuid == v) {
				return names[i].name;
			}
		}
	}
	bt_uuid_to_str(uuid, buf, len);
	return buf;
}

static uint8_t find_attr_cb(const struct bt_gatt_attr *attr, uint16_t handle, void *user_data)
{
	const struct bt_gatt_attr **out = user_data;

	ARG_UNUSED(handle);
	*out = attr;
	return BT_GATT_ITER_STOP;
}

static void trace(struct bt_conn *conn, const struct bt_gatt_attr *attr, bool write)
{
	const uint16_t handle = bt_gatt_attr_get_handle(attr);
	const uint8_t ci = bt_conn_index(conn);
	char buf[BT_UUID_STR_LEN];
	char pbuf[BT_UUID_STR_LEN];

	if (!atomic_get(&enabled)) {
		return;
	}
	/* Khai bao dich vu / dac tinh: dien thoai dang do tim, khong phai doc gia tri */
	if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CHRC) ||
	    !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_PRIMARY) ||
	    !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_SECONDARY) ||
	    !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_INCLUDE)) {
		return;
	}
	if (!write) {
		const int64_t now = k_uptime_get();

		if (handle == last_handle && ci == last_conn && now - last_read_ms < READ_DEDUP_MS) {
			return;
		}
		last_handle = handle;
		last_conn = ci;
		last_read_ms = now;
	} else {
		last_handle = 0;
	}

	if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) {
		/* CCC dung ngay sau gia tri cua dac tinh no thuoc ve */
		const struct bt_gatt_attr *parent = NULL;

		if (handle > 1) {
			bt_gatt_foreach_attr(handle - 1, handle - 1, find_attr_cb, &parent);
		}
		LOG_INF("conn %u %s h0x%04x CCC of %s", ci, write ? "WRITE" : "read ", handle,
			parent ? uuid_name(parent->uuid, pbuf, sizeof(pbuf)) : "?");
		return;
	}

	LOG_INF("conn %u %s h0x%04x %s", ci, write ? "WRITE" : "read ", handle,
		uuid_name(attr->uuid, buf, sizeof(buf)));
}

static bool read_authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	trace(conn, attr, false);
	return true;
}

static bool write_authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	trace(conn, attr, true);
	return true;
}

static const struct bt_gatt_authorization_cb auth_cb = {
	.read_authorize = read_authorize,
	.write_authorize = write_authorize,
};

/* ATT MTU: Receive State / PAC dai hon MTU - 3 thi dien thoai phai doc dai */
static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("conn %u ATT MTU: tx %u, rx %u", bt_conn_index(conn), tx, rx);
}

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = mtu_updated,
};

int gatt_trace_init(void)
{
	int err = bt_gatt_authorization_cb_register(&auth_cb);

	if (err) {
		LOG_ERR("GATT trace register failed (%d)", err);
		return err;
	}
	bt_gatt_cb_register(&gatt_cb);
	LOG_INF("GATT access trace ON (diagnostic build) - \"helmet trace off\" to silence");
	return 0;
}

int gatt_trace_shell(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			atomic_set(&enabled, 1);
			last_handle = 0;
		} else if (strcmp(argv[1], "off") == 0) {
			atomic_set(&enabled, 0);
		} else {
			shell_error(sh, "usage: helmet trace [on|off]");
			return -EINVAL;
		}
	}
	shell_print(sh, "GATT access trace %s", atomic_get(&enabled) ? "ON" : "OFF");
	return 0;
}
