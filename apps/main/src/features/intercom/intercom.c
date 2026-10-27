/*
 * Helmet - intercom mu-mu, buoc S1: chon vai tro bang nut, ket noi, ghep doi, bat tay HIS,
 * ket thuc, noi lai khi mat song. Am thanh (BAP Unicast Client) o buoc S2.
 * Thiet ke: docs/HELMET_THIET_KE_INTERCOM.md (ban master/slave, vai tro chon bang nut)
 *
 * Buoc S2: ngay khi LINKED, master mo duong am thanh hai chieu (ic_audio.c, BAP Unicast Client);
 * chu ky ket noi giu 15 ms toi khi luong chay roi moi doi 60 ms.
 *
 * Binh thuong mu khong co vai tro. Power nhan ngan roi giu Vol+ 2 s -> MASTER, giu Vol- 2 s
 * -> SLAVE. Vai tro chi nam trong RAM: ket thuc / tat nguon -> ve binh thuong. Bond van luu.
 *
 *  MASTER: quet tim mu co co IC_SLAVE trong quang ba, gom 0.6 s, chon RSSI manh nhat, ket noi
 *          (chu ky 15 ms), ma hoa (ghep Just Works lan dau), do HIS, gui HELLO.
 *  SLAVE:  them co IC vao quang ba. Ket noi den trong luc cho la "ung vien": gui HELLO trong
 *          5 s sau khi ma hoa -> la mu ban; khong -> giao cho luong dien thoai.
 *  LINKED: Call ngan -> END -> ngat. Mat song (khong phai ngat chu dong) -> 60 s tim lai dung
 *          mu ban (master quet, slave quang ba), qua 60 s -> ve binh thuong.
 *
 * Moi trang thai chi doi trong thread bo dieu khien (intercom_handle). Callback Bluetooth va
 * hen gio chi dua viec vao ic_q roi bao EVT_IC.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include <me54/services/audio_out.h>
#include <me54/services/voice_tx.h>

#include "app/app.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/nav/nav.h"
#include "features/phone/le_audio.h"

LOG_MODULE_REGISTER(intercom, LOG_LEVEL_INF);

#define IC_MODE_TIMEOUT_S 60  /* tim / cho / noi lai toi da */
#define IC_CHOOSE_MS      600 /* gom ung vien sau lan thay dau tien, chon RSSI manh nhat */
#define IC_LINK_TIMEOUT_S 10  /* master: tu luc tao ket noi toi khi nhan HELLO */
#define IC_HELLO_WAIT_S   5   /* slave: ung vien da ma hoa ma chua gui HELLO */
#define IC_CAND_TIMEOUT_S 15  /* slave: ung vien ket noi ma khong ma hoa */
#define IC_END_WAIT_MS    300 /* cho END toi noi truoc khi ngat */
#define IC_PEER_END_MS    1000
#define IC_RSSI_MIN       (-85)
#define IC_MAX_PEERS      4
#define IC_MAX_CAND       2
#define IC_MAX_BAD        2
#define IC_SETUP_INT      12  /* x 1.25 ms = 15 ms: thiet lap nhanh */
#define IC_LINKED_INT     48  /* 60 ms khi da xong */
#define IC_SUP_TIMEOUT    400 /* x 10 ms = 4 s: phat hien mat song */
#define KEEP_PROMPT       (-2)
#define STOP_PROMPT       (-1)

enum qev {
	QE_CONNECTED,    /* a = loi HCI, b = 1 neu la ung vien (slave) */
	QE_SECURED,      /* a = 1 neu loi, b = muc bao mat */
	QE_DISCONNECTED, /* a = ly do */
	QE_DISCOVERED,   /* a = 1 neu loi */
	QE_MSG,          /* a = op, b = arg */
	QE_T_MODE,
	QE_T_CHOOSE,
	QE_T_LINK,
	QE_T_HELLO,
	QE_T_END,
	QE_CMD,   /* a = enum qcmd (lenh shell) */
	QE_AUDIO, /* a = su kien ic_audio.c, b = tham so */
};

enum qcmd { CMD_MASTER, CMD_SLAVE, CMD_STOP, CMD_UNPAIR };

struct qitem {
	uint8_t ev;
	uint8_t a;
	uint8_t b;
	struct bt_conn *conn; /* giu 1 ref, nha sau khi xu ly */
};

K_MSGQ_DEFINE(ic_q, sizeof(struct qitem), 16, 4);

static void q_post(uint8_t ev, uint8_t a, uint8_t b, struct bt_conn *conn)
{
	struct qitem it = {.ev = ev, .a = a, .b = b, .conn = conn ? bt_conn_ref(conn) : NULL};

	if (k_msgq_put(&ic_q, &it, K_NO_WAIT) != 0) {
		LOG_ERR("Intercom queue full (event %u lost)", ev);
		if (it.conn != NULL) {
			bt_conn_unref(it.conn);
		}
		return;
	}
	app_post(EVT_IC, 0, 0, 0);
}

static void t_mode_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_T_MODE, 0, 0, NULL);
}

static void t_choose_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_T_CHOOSE, 0, 0, NULL);
}

static void t_link_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_T_LINK, 0, 0, NULL);
}

static void t_hello_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_T_HELLO, 0, 0, NULL);
}

static void t_end_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_T_END, 0, 0, NULL);
}

static K_TIMER_DEFINE(t_mode, t_mode_fn, NULL);
static K_TIMER_DEFINE(t_choose, t_choose_fn, NULL);
static K_TIMER_DEFINE(t_link, t_link_fn, NULL);
static K_TIMER_DEFINE(t_hello, t_hello_fn, NULL);
static K_TIMER_DEFINE(t_end, t_end_fn, NULL);

/* ---------------- Trang thai doc duoc tu moi thread ---------------- */

static atomic_t mode_a = ATOMIC_INIT(ICM_NORMAL);
static atomic_t accepting;  /* slave dang cho: nhan ket noi den lam ung vien */
static atomic_t restrict_a; /* noi lai: chi chap nhan dung peer_id */
static bt_addr_le_t peer_id; /* ghi truoc khi bat restrict_a */
static bt_addr_le_t bad[IC_MAX_BAD]; /* khong co HIS trong lan tim nay (ghi khi khong quet) */
static uint8_t n_bad;

/* Ket noi do intercom quan ly: bt_rx them/xoa, cac thread khac tra cuu */
static struct k_spinlock own_lock;
static struct bt_conn *owned[IC_MAX_CAND + 1];

/* Ung vien tot nhat khi quet (master) */
static struct k_spinlock scan_lock;
static bool best_valid;
static bt_addr_le_t best_addr;
static int8_t best_rssi;

/* ---------------- Trang thai chi thread bo dieu khien ---------------- */

static enum ic_mode mode = ICM_NORMAL;
static enum ic_role role;
static bool reconnecting;
static bool relaxed; /* master: da doi chu ky ket noi sang 60 ms sau khi luong chay */
static struct bt_conn *link;               /* ket noi mu-mu (giu ref) */
static struct bt_conn *cand[IC_MAX_CAND]; /* slave: ung vien (giu ref) */

/* Cac mu da tung ghep (settings "ic/peers"): de "helmet ic unpair" chi xoa bond cua mu */
static bt_addr_le_t peers[IC_MAX_PEERS];
static uint8_t n_peers;

/* ---------------- Tien ich ---------------- */

static void id_norm(bt_addr_le_t *dst, const bt_addr_le_t *src)
{
	bt_addr_le_copy(dst, src);
	if (dst->type == BT_ADDR_LE_PUBLIC_ID) {
		dst->type = BT_ADDR_LE_PUBLIC;
	} else if (dst->type == BT_ADDR_LE_RANDOM_ID) {
		dst->type = BT_ADDR_LE_RANDOM;
	}
}

static bool conn_is(const struct bt_conn *conn, const bt_addr_le_t *id)
{
	bt_addr_le_t dst;

	id_norm(&dst, bt_conn_get_dst(conn));
	return bt_addr_le_eq(&dst, id);
}

static const char *conn_str(struct bt_conn *conn, char *buf, size_t len)
{
	bt_addr_le_to_str(bt_conn_get_dst(conn), buf, len);
	return buf;
}

static void own_add(struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&own_lock);

	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] == NULL) {
			owned[i] = bt_conn_ref(conn);
			break;
		}
	}
	k_spin_unlock(&own_lock, key);
}

static void own_remove(const struct bt_conn *conn)
{
	struct bt_conn *found = NULL;
	k_spinlock_key_t key = k_spin_lock(&own_lock);

	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] == conn) {
			found = owned[i];
			owned[i] = NULL;
			break;
		}
	}
	k_spin_unlock(&own_lock, key);

	if (found != NULL) {
		bt_conn_unref(found);
	}
}

static int find_cand(const struct bt_conn *conn)
{
	for (int i = 0; i < IC_MAX_CAND; i++) {
		if (cand[i] != NULL && cand[i] == conn) {
			return i;
		}
	}
	return -1;
}

static bool is_bad(const bt_addr_le_t *addr)
{
	for (uint8_t i = 0; i < n_bad && i < IC_MAX_BAD; i++) {
		if (bt_addr_le_eq(&bad[i], addr)) {
			return true;
		}
	}
	return false;
}

const char *intercom_mode_str(enum ic_mode m)
{
	switch (m) {
	case ICM_NORMAL:
		return "NORMAL";
	case ICM_SEARCH:
		return "SEARCH";
	case ICM_WAIT:
		return "WAIT";
	case ICM_LINKING:
		return "LINKING";
	case ICM_LINKED:
		return "LINKED";
	case ICM_ENDING:
		return "ENDING";
	default:
		return "?";
	}
}

static const char *role_str(enum ic_role r)
{
	return r == IC_ROLE_MASTER ? "MASTER" : (r == IC_ROLE_SLAVE ? "SLAVE" : "none");
}

enum ic_mode intercom_mode(void)
{
	return (enum ic_mode)atomic_get(&mode_a);
}

bool intercom_adv_ic(void)
{
	return atomic_get(&accepting) != 0;
}

/* ---------------- Danh sach mu da ghep ---------------- */

static int peers_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	ssize_t r;

	if (!settings_name_steq(key, "peers", &next) || next) {
		return -ENOENT;
	}
	if (len > sizeof(peers) || (len % sizeof(bt_addr_le_t)) != 0) {
		return -EINVAL;
	}
	r = read_cb(cb_arg, peers, len);
	if (r < 0) {
		return (int)r;
	}
	n_peers = (uint8_t)(r / sizeof(bt_addr_le_t));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ic, "ic", NULL, peers_set, NULL, NULL);

static void peers_add(const bt_addr_le_t *id)
{
	for (uint8_t i = 0; i < n_peers; i++) {
		if (bt_addr_le_eq(&peers[i], id)) {
			return;
		}
	}
	if (n_peers == IC_MAX_PEERS) {
		memmove(&peers[0], &peers[1], (IC_MAX_PEERS - 1) * sizeof(peers[0]));
		n_peers--;
	}
	bt_addr_le_copy(&peers[n_peers++], id);
	(void)settings_save_one("ic/peers", peers, n_peers * sizeof(peers[0]));
}

/* ---------------- Quet (master) ---------------- */

static bool ad_has_ic_slave(struct bt_data *data, void *user_data)
{
	static const uint8_t uuid[] = {IC_UUID_SVC_VAL};
	bool *found = user_data;

	if (data->type == BT_DATA_SVC_DATA128 && data->data_len >= sizeof(uuid) + 2 &&
	    memcmp(data->data, uuid, sizeof(uuid)) == 0 && data->data[16] == IC_PROTO_VER &&
	    (data->data[17] & IC_ADV_FLAG_SLAVE) != 0) {
		*found = true;
		return false;
	}
	return true;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	struct net_buf_simple_state state;
	k_spinlock_key_t key;
	bt_addr_le_t addr;
	bool found = false;
	bool first;

	if (atomic_get(&mode_a) != ICM_SEARCH || info->rssi < IC_RSSI_MIN ||
	    (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) == 0) {
		return;
	}

	net_buf_simple_save(buf, &state);
	bt_data_parse(buf, ad_has_ic_slave, &found);
	net_buf_simple_restore(buf, &state);
	if (!found) {
		return;
	}

	id_norm(&addr, info->addr);
	if ((atomic_get(&restrict_a) && !bt_addr_le_eq(&addr, &peer_id)) || is_bad(&addr)) {
		return;
	}

	key = k_spin_lock(&scan_lock);
	first = !best_valid;
	if (first || info->rssi > best_rssi) {
		best_valid = true;
		bt_addr_le_copy(&best_addr, &addr);
		best_rssi = info->rssi;
	}
	k_spin_unlock(&scan_lock, key);

	if (first) {
		k_timer_start(&t_choose, K_MSEC(IC_CHOOSE_MS), K_NO_WAIT);
	}
}

static struct bt_le_scan_cb scan_cb = {
	.recv = scan_recv,
};

/* ---------------- Chuyen trang thai ---------------- */

static void set_mode(enum ic_mode m)
{
	if (m == mode) {
		return;
	}
	LOG_INF("Intercom %s -> %s", intercom_mode_str(mode), intercom_mode_str(m));
	mode = m;
	atomic_set(&mode_a, m);
	atomic_set(&accepting, m == ICM_WAIT);
	app_adv_refresh();
}

static int start_search(void)
{
	static const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0030, /* 30 ms */
		.window = 0x0030,   /* quet lien tuc trong luc tim */
	};
	k_spinlock_key_t key = k_spin_lock(&scan_lock);
	int err;

	best_valid = false;
	k_spin_unlock(&scan_lock, key);

	set_mode(ICM_SEARCH);
	err = bt_le_scan_start(&param, NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("Scan start failed (%d)", err);
		return err;
	}
	return 0;
}

static void stop_scan(void)
{
	k_timer_stop(&t_choose);
	(void)bt_le_scan_stop();
}

static void drop_link(uint8_t reason)
{
	if (link != NULL) {
		(void)bt_conn_disconnect(link, reason);
		bt_conn_unref(link);
		link = NULL;
	}
}

/* Ung vien khong phai mu ban: giao cho luong dien thoai (app/app_bt.c tu ngat neu da co) */
static void release_cand(int i)
{
	struct bt_conn *c = cand[i];

	cand[i] = NULL;
	if (c != NULL) {
		app_phone_adopt(c); /* chuyen luon ref */
	}
}

static void go_normal(int prompt)
{
	k_timer_stop(&t_mode);
	k_timer_stop(&t_link);
	k_timer_stop(&t_hello);
	k_timer_stop(&t_end);
	if (mode == ICM_SEARCH) {
		stop_scan();
	}
	atomic_set(&restrict_a, 0);
	ic_audio_conn_lost();
	drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	for (int i = 0; i < IC_MAX_CAND; i++) {
		release_cand(i);
	}
	role = IC_ROLE_NONE;
	reconnecting = false;
	set_mode(ICM_NORMAL);

	if (prompt >= 0) {
		audio_out_prompt((enum prompt_id)prompt);
	} else if (prompt == STOP_PROMPT) {
		audio_out_prompt_stop();
	}
	/* Khong co lien ket nao: hen gio tu tat chay lai tu dau */
	if (app_state() == HS_IDLE) {
		power_activity();
	}
}

/* Master: thu lai trong thoi gian con lai cua che do; het gio -> ve binh thuong */
static void retry_search(void)
{
	k_timer_stop(&t_link);
	if (k_timer_remaining_get(&t_mode) == 0 || start_search() != 0) {
		go_normal(reconnecting ? PROMPT_IC_LOST : PROMPT_IC_UNAVAIL);
	}
}

static void linked(void)
{
	char addr[BT_ADDR_LE_STR_LEN];

	k_timer_stop(&t_mode);
	k_timer_stop(&t_link);
	k_timer_stop(&t_hello);
	atomic_set(&restrict_a, 0);
	id_norm(&peer_id, bt_conn_get_dst(link));
	peers_add(&peer_id);
	set_mode(ICM_LINKED);

	bt_addr_le_to_str(&peer_id, addr, sizeof(addr));
	LOG_INF("Intercom LINKED with %s as %s%s", addr, role_str(role),
		reconnecting ? " (reconnected)" : "");
	reconnecting = false;
	audio_out_prompt(PROMPT_IC_LINKED);

	if (role == IC_ROLE_MASTER) {
		/* Mo duong am thanh ngay (ca hai da chu dong bam). Chu ky 15 ms giu toi khi xong. */
		relaxed = false;
		(void)ic_audio_start(link);
	}
	/* Slave: cac ung vien khac (vd dien thoai ket noi cung luc) giao lai ngay */
	for (int i = 0; i < IC_MAX_CAND; i++) {
		release_cand(i);
	}
}

static void end_link(uint8_t reason)
{
	int err = -ENOTCONN;

	LOG_INF("Intercom: ending (reason %u)", reason);
	audio_out_prompt(PROMPT_IC_END);
	set_mode(ICM_ENDING);
	if (link != NULL) {
		err = ic_svc_send(link, role == IC_ROLE_MASTER, IC_OP_END, reason);
	}
	k_timer_start(&t_end, K_MSEC(err ? 1 : IC_END_WAIT_MS), K_NO_WAIT);
}

static void start_link_loss_recovery(uint8_t reason)
{
	ic_audio_conn_lost();
	LOG_WRN("Intercom: link lost (0x%02x %s) - looking for peer for %d s", reason,
		bt_hci_err_to_str(reason), IC_MODE_TIMEOUT_S);
	reconnecting = true;
	atomic_set(&restrict_a, 1);
	if (role == IC_ROLE_MASTER) {
		n_bad = 0;
		if (start_search() != 0) {
			go_normal(PROMPT_IC_LOST);
			return;
		}
	} else {
		set_mode(ICM_WAIT);
	}
	k_timer_start(&t_mode, K_SECONDS(IC_MODE_TIMEOUT_S), K_NO_WAIT);
}

/* ---------------- Lenh tu nut / shell ---------------- */

void intercom_select(enum ic_role r)
{
	const enum helmet_state s = app_state();

	if (mode != ICM_NORMAL) {
		LOG_WRN("Intercom busy (%s) - ignoring role selection", intercom_mode_str(mode));
		return;
	}
	if (nav_active()) {
		/* Dinh vi co the dang giu lien ket toi chinh mu ban (moi cap thiet bi 1 ket noi) */
		LOG_WRN("Intercom not available in navigation mode - turn it off first (hold Call 1 s)");
		return;
	}
	if (s == HS_RINGING || s == HS_IN_CALL || s == HS_SHUTDOWN || s == HS_BOOT) {
		LOG_WRN("Intercom not available in state %s", app_state_str(s));
		return;
	}

	/* Auracast va intercom loai tru nhau (CPU, radio, khe giai ma): roi broadcast truoc */
	if (bcast_active()) {
		LOG_INF("Intercom: leaving Auracast broadcast first");
		bcast_leave();
	}

	role = r;
	reconnecting = false;
	atomic_set(&restrict_a, 0);
	n_bad = 0;

	if (r == IC_ROLE_MASTER) {
		LOG_INF("Intercom: MASTER - searching for a slave helmet (%d s)", IC_MODE_TIMEOUT_S);
		if (start_search() != 0) {
			go_normal(PROMPT_IC_UNAVAIL);
			return;
		}
		audio_out_prompt(PROMPT_IC_SEARCH);
	} else {
		LOG_INF("Intercom: SLAVE - waiting for a master helmet (%d s)", IC_MODE_TIMEOUT_S);
		set_mode(ICM_WAIT);
		audio_out_prompt(PROMPT_IC_WAIT);
	}
	k_timer_start(&t_mode, K_SECONDS(IC_MODE_TIMEOUT_S), K_NO_WAIT);
}

void intercom_stop(void)
{
	switch (mode) {
	case ICM_SEARCH:
	case ICM_WAIT:
	case ICM_LINKING:
		LOG_INF("Intercom cancelled");
		go_normal(PROMPT_IC_END);
		break;
	case ICM_LINKED:
		end_link(IC_REASON_USER);
		break;
	default:
		break;
	}
}

static void do_unpair(void)
{
	if (mode != ICM_NORMAL) {
		go_normal(STOP_PROMPT);
	}
	for (uint8_t i = 0; i < n_peers; i++) {
		char addr[BT_ADDR_LE_STR_LEN];
		const int err = bt_unpair(BT_ID_DEFAULT, &peers[i]);

		bt_addr_le_to_str(&peers[i], addr, sizeof(addr));
		LOG_INF("Intercom: unpair %s (%d)", addr, err);
	}
	n_peers = 0;
	(void)settings_delete("ic/peers");
}

/* ---------------- Xu ly su kien (thread bo dieu khien) ---------------- */

static void on_connected(const struct qitem *it)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	if (it->b) {
		/* Slave: ket noi den trong luc cho */
		int i;

		if (mode != ICM_WAIT) {
			app_phone_adopt(bt_conn_ref(it->conn));
			return;
		}
		for (i = 0; i < IC_MAX_CAND && cand[i] != NULL; i++) {
		}
		if (i == IC_MAX_CAND) {
			(void)bt_conn_disconnect(it->conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
			return;
		}
		cand[i] = bt_conn_ref(it->conn);
		LOG_INF("Intercom: incoming connection %s (candidate)",
			conn_str(it->conn, addr, sizeof(addr)));
		(void)bt_conn_set_security(it->conn, BT_SECURITY_L2);
		k_timer_start(&t_hello, K_SECONDS(IC_CAND_TIMEOUT_S), K_NO_WAIT);
		return;
	}

	/* Master: ket qua tao ket noi */
	if (it->conn != link || mode != ICM_LINKING) {
		return;
	}
	if (it->a) {
		LOG_WRN("Intercom: connection failed (0x%02x %s)", it->a, bt_hci_err_to_str(it->a));
		bt_conn_unref(link);
		link = NULL;
		retry_search();
		return;
	}
	LOG_INF("Intercom: connected to %s, securing", conn_str(link, addr, sizeof(addr)));
	err = bt_conn_set_security(link, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("Intercom: security request failed (%d)", err);
		drop_link(BT_HCI_ERR_AUTH_FAIL);
		retry_search();
	}
}

static void on_secured(const struct qitem *it)
{
	int i;
	int err;

	if (it->conn == link && mode == ICM_LINKING) {
		if (it->a) {
			LOG_WRN("Intercom: security failed - disconnecting");
			drop_link(BT_HCI_ERR_AUTH_FAIL);
			retry_search();
			return;
		}
		LOG_INF("Intercom: link encrypted (level %u), looking for HIS", it->b);
		err = ic_svc_discover(link);
		if (err) {
			LOG_WRN("Intercom: HIS discovery failed (%d)", err);
			drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			retry_search();
		}
		return;
	}

	i = find_cand(it->conn);
	if (i < 0) {
		return;
	}
	if (it->a) {
		(void)bt_conn_disconnect(cand[i], BT_HCI_ERR_AUTH_FAIL);
		bt_conn_unref(cand[i]);
		cand[i] = NULL;
		return;
	}
	/* Master that se gui HELLO ngay sau khi do xong HIS (~0.3 s) */
	k_timer_start(&t_hello, K_SECONDS(IC_HELLO_WAIT_S), K_NO_WAIT);
}

static void on_discovered(const struct qitem *it)
{
	int err;

	if (it->conn != link || mode != ICM_LINKING) {
		return;
	}
	if (it->a) {
		LOG_WRN("Intercom: peer has no Helmet Intercom Service - not a helmet");
		if (n_bad < IC_MAX_BAD) {
			id_norm(&bad[n_bad++], bt_conn_get_dst(link));
		}
		drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		retry_search();
		return;
	}
	err = ic_svc_send(link, true, IC_OP_HELLO, IC_PROTO_VER);
	if (err) {
		LOG_WRN("Intercom: HELLO send failed (%d)", err);
		drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		retry_search();
		return;
	}
	LOG_INF("Intercom: HELLO sent");
}

static void reject(struct bt_conn *conn, uint8_t reason)
{
	(void)ic_svc_send(conn, false, IC_OP_END, reason);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void on_msg(const struct qitem *it)
{
	int i;

	switch (it->a) {
	case IC_OP_HELLO:
		if (role == IC_ROLE_MASTER) {
			/* Slave tra HELLO qua Event */
			if (it->conn != link || mode != ICM_LINKING) {
				return;
			}
			if (it->b != IC_PROTO_VER) {
				LOG_WRN("Intercom: peer protocol v%u, need v%u", it->b, IC_PROTO_VER);
				drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				go_normal(PROMPT_IC_UNAVAIL);
				return;
			}
			linked();
			return;
		}

		/* Slave: HELLO cua master qua Control Point */
		i = find_cand(it->conn);
		if (mode == ICM_WAIT && i >= 0) {
			if (it->b != IC_PROTO_VER) {
				LOG_WRN("Intercom: master protocol v%u, need v%u", it->b, IC_PROTO_VER);
				reject(it->conn, IC_REASON_VERSION);
				return;
			}
			if (atomic_get(&restrict_a) && !conn_is(it->conn, &peer_id)) {
				LOG_WRN("Intercom: HELLO from another helmet while reconnecting - rejected");
				reject(it->conn, IC_REASON_NOT_ALLOWED);
				return;
			}
			link = cand[i]; /* chuyen ref */
			cand[i] = NULL;
			role = IC_ROLE_SLAVE;
			(void)ic_svc_send(link, false, IC_OP_HELLO, IC_PROTO_VER);
			linked();
			return;
		}
		if (it->conn != link) {
			LOG_WRN("Intercom: unexpected HELLO (mode %s) - rejected", intercom_mode_str(mode));
			reject(it->conn, IC_REASON_NOT_ALLOWED);
		}
		return;

	case IC_OP_END:
		if (it->conn == link && mode == ICM_LINKED) {
			LOG_INF("Intercom: peer ended (reason %u)", it->b);
			audio_out_prompt(PROMPT_IC_END);
			set_mode(ICM_ENDING);
			/* Ben kia tu ngat; khong thay thi minh ngat */
			k_timer_start(&t_end, K_MSEC(IC_PEER_END_MS), K_NO_WAIT);
		}
		return;

	default:
		LOG_DBG("Intercom: unknown op 0x%02x", it->a);
		return;
	}
}

static void on_disconnected(const struct qitem *it)
{
	const uint8_t reason = it->a;
	const int i = find_cand(it->conn);

	if (i >= 0) {
		bt_conn_unref(cand[i]);
		cand[i] = NULL;
		return;
	}
	if (link == NULL || it->conn != link) {
		return; /* ket noi da duoc bo truoc do */
	}
	bt_conn_unref(link);
	link = NULL;

	switch (mode) {
	case ICM_ENDING:
		go_normal(KEEP_PROMPT);
		break;
	case ICM_LINKING:
		LOG_WRN("Intercom: link setup lost (0x%02x %s)", reason, bt_hci_err_to_str(reason));
		retry_search();
		break;
	case ICM_LINKED:
		if (reason == BT_HCI_ERR_REMOTE_USER_TERM_CONN ||
		    reason == BT_HCI_ERR_REMOTE_POWER_OFF ||
		    reason == BT_HCI_ERR_LOCALHOST_TERM_CONN) {
			LOG_INF("Intercom: peer disconnected (0x%02x %s)", reason,
				bt_hci_err_to_str(reason));
			go_normal(PROMPT_IC_END);
		} else {
			start_link_loss_recovery(reason);
		}
		break;
	default:
		break;
	}
}

static void on_choose(void)
{
	char str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_t addr;
	k_spinlock_key_t key;
	bool valid;
	int8_t rssi;
	int err;

	if (mode != ICM_SEARCH) {
		return;
	}
	key = k_spin_lock(&scan_lock);
	valid = best_valid;
	bt_addr_le_copy(&addr, &best_addr);
	rssi = best_rssi;
	best_valid = false;
	k_spin_unlock(&scan_lock, key);
	if (!valid) {
		return;
	}

	stop_scan();
	bt_addr_le_to_str(&addr, str, sizeof(str));
	LOG_INF("Intercom: connecting to %s (RSSI %d dBm)", str, rssi);
	err = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM(IC_SETUP_INT, IC_SETUP_INT, 0, IC_SUP_TIMEOUT),
				&link);
	if (err) {
		LOG_WRN("Intercom: create connection failed (%d)", err);
		link = NULL;
		retry_search();
		return;
	}
	set_mode(ICM_LINKING);
	k_timer_start(&t_link, K_SECONDS(IC_LINK_TIMEOUT_S), K_NO_WAIT);
}

static void on_timer_mode(void)
{
	switch (mode) {
	case ICM_SEARCH:
	case ICM_WAIT:
	case ICM_LINKING:
		LOG_INF("Intercom: no peer within %d s", IC_MODE_TIMEOUT_S);
		go_normal(reconnecting ? PROMPT_IC_LOST : PROMPT_IC_UNAVAIL);
		break;
	default:
		break;
	}
}

static void on_timer_hello(void)
{
	for (int i = 0; i < IC_MAX_CAND; i++) {
		if (cand[i] != NULL) {
			char addr[BT_ADDR_LE_STR_LEN];

			LOG_INF("Intercom: %s sent no HELLO - not a helmet, handing over",
				conn_str(cand[i], addr, sizeof(addr)));
			release_cand(i);
		}
	}
}

static void on_cmd(uint8_t cmd)
{
	switch (cmd) {
	case CMD_MASTER:
		intercom_select(IC_ROLE_MASTER);
		break;
	case CMD_SLAVE:
		intercom_select(IC_ROLE_SLAVE);
		break;
	case CMD_STOP:
		intercom_stop();
		break;
	case CMD_UNPAIR:
		do_unpair();
		break;
	default:
		break;
	}
}

void intercom_handle(void)
{
	struct qitem it;

	while (k_msgq_get(&ic_q, &it, K_NO_WAIT) == 0) {
		switch (it.ev) {
		case QE_CONNECTED:
			on_connected(&it);
			break;
		case QE_SECURED:
			on_secured(&it);
			break;
		case QE_DISCONNECTED:
			on_disconnected(&it);
			break;
		case QE_DISCOVERED:
			on_discovered(&it);
			break;
		case QE_MSG:
			on_msg(&it);
			break;
		case QE_T_MODE:
			on_timer_mode();
			break;
		case QE_T_CHOOSE:
			on_choose();
			break;
		case QE_T_LINK:
			if (mode == ICM_LINKING) {
				LOG_WRN("Intercom: link setup took > %d s", IC_LINK_TIMEOUT_S);
				drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				retry_search();
			}
			break;
		case QE_T_HELLO:
			on_timer_hello();
			break;
		case QE_T_END:
			if (mode == ICM_ENDING) {
				drop_link(BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				go_normal(KEEP_PROMPT);
			}
			break;
		case QE_CMD:
			on_cmd(it.a);
			break;
		case QE_AUDIO:
			ic_audio_on_event(it.a, it.b);
			if (role == IC_ROLE_MASTER && link != NULL && !relaxed &&
			    ic_audio_streaming()) {
				relaxed = true;
				(void)bt_conn_le_param_update(
					link,
					BT_LE_CONN_PARAM(IC_LINKED_INT, IC_LINKED_INT, 0, IC_SUP_TIMEOUT));
			}
			break;
		default:
			break;
		}
		if (it.conn != NULL) {
			bt_conn_unref(it.conn);
		}
	}
}

/* ---------------- Callback Bluetooth (bt_rx) tu app/app_bt.c / ic_service.c ---------------- */

bool intercom_conn_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0) {
		return false;
	}
	if (info.role == BT_CONN_ROLE_CENTRAL) {
		/* Chi intercom (master) tao ket noi central */
		if (err == 0) {
			own_add(conn);
		}
		q_post(QE_CONNECTED, err, 0, conn);
		return true;
	}
	if (err != 0 || !atomic_get(&accepting)) {
		return false;
	}
	own_add(conn);
	q_post(QE_CONNECTED, 0, 1, conn);
	return true;
}

bool intercom_conn_owned(const struct bt_conn *conn)
{
	bool found = false;
	k_spinlock_key_t key = k_spin_lock(&own_lock);

	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] != NULL && owned[i] == conn) {
			found = true;
			break;
		}
	}
	k_spin_unlock(&own_lock, key);
	return found;
}

void intercom_conn_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	q_post(QE_SECURED, err ? 1 : 0, (uint8_t)level, conn);
}

void intercom_conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
	own_remove(conn);
	q_post(QE_DISCONNECTED, reason, 0, conn);
}

void intercom_conn_release(struct bt_conn *conn)
{
	own_remove(conn);
}

void intercom_on_msg(struct bt_conn *conn, uint8_t op, uint8_t arg)
{
	q_post(QE_MSG, op, arg, conn);
}

void intercom_on_discovered(struct bt_conn *conn, int err)
{
	q_post(QE_DISCOVERED, err ? 1 : 0, 0, conn);
}

void intercom_on_audio(uint8_t ev, uint8_t arg)
{
	q_post(QE_AUDIO, ev, arg, NULL);
}

/* ---------------- Tat may / shell / khoi tao ---------------- */

void intercom_shutdown(void)
{
	k_timer_stop(&t_mode);
	k_timer_stop(&t_link);
	k_timer_stop(&t_hello);
	k_timer_stop(&t_end);
	if (mode == ICM_SEARCH) {
		stop_scan();
	}
	ic_audio_conn_lost();
	if (link != NULL && mode == ICM_LINKED &&
	    ic_svc_send(link, role == IC_ROLE_MASTER, IC_OP_END, IC_REASON_POWER_OFF) == 0) {
		for (int i = 0; i < IC_END_WAIT_MS / 10 && ic_svc_busy(); i++) {
			k_msleep(10);
		}
	}
	drop_link(BT_HCI_ERR_REMOTE_POWER_OFF);
	for (int i = 0; i < IC_MAX_CAND; i++) {
		if (cand[i] != NULL) {
			(void)bt_conn_disconnect(cand[i], BT_HCI_ERR_REMOTE_POWER_OFF);
			bt_conn_unref(cand[i]);
			cand[i] = NULL;
		}
	}
	role = IC_ROLE_NONE;
	mode = ICM_NORMAL;
	atomic_set(&mode_a, ICM_NORMAL);
	atomic_set(&accepting, 0);
}

void intercom_log_status(void)
{
	char addr[BT_ADDR_LE_STR_LEN] = "-";

	if (mode == ICM_NORMAL) {
		return;
	}
	if (link != NULL) {
		conn_str(link, addr, sizeof(addr));
	}
	LOG_INF("intercom: %s, role %s, peer %s, audio %s%s", intercom_mode_str(mode),
		role_str(role), addr,
		role == IC_ROLE_MASTER ? ic_audio_state_str()
				       : (le_audio_is_streaming() ? "STREAMING (as slave)" : "idle"),
		reconnecting ? ", reconnecting" : "");
}

int intercom_shell(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const cmds[] = {"master", "slave", "stop", "unpair"};
	struct bt_conn_info info;
	char addr[BT_ADDR_LE_STR_LEN];

	if (argc > 1 && strcmp(argv[1], "tone") == 0) {
		const unsigned long hz =
			(argc > 2 && strcmp(argv[2], "off") != 0) ? strtoul(argv[2], NULL, 10) : 0;

		if (hz > 7000) {
			shell_error(sh, "usage: helmet ic tone <50..7000 Hz>|off");
			return -EINVAL;
		}
		voice_set_tone((uint16_t)hz);
		shell_print(sh, "Test tone %s", hz ? "on (replaces microphone)" : "off");
		return 0;
	}

	if (argc > 1) {
		for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
			if (strcmp(argv[1], cmds[i]) == 0) {
				q_post(QE_CMD, (uint8_t)i, 0, NULL); /* xu ly trong thread bo dieu khien */
				return 0;
			}
		}
		if (strcmp(argv[1], "status") != 0) {
			shell_error(sh, "usage: helmet ic [status|master|slave|stop|unpair|tone <Hz>|off]");
			return -EINVAL;
		}
	}

	shell_print(sh, "Intercom  : %s, role %s%s", intercom_mode_str(intercom_mode()),
		    role_str(role), reconnecting ? ", reconnecting" : "");
	if (link != NULL && bt_conn_get_info(link, &info) == 0) {
		shell_print(sh, "Link      : %s, %s, interval %u us, security %d",
			    conn_str(link, addr, sizeof(addr)),
			    info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral",
			    info.le.interval_us, bt_conn_get_security(link));
	}
	for (int i = 0; i < IC_MAX_CAND; i++) {
		if (cand[i] != NULL) {
			shell_print(sh, "Candidate : %s", conn_str(cand[i], addr, sizeof(addr)));
		}
	}
	if (role == IC_ROLE_MASTER) {
		ic_audio_print(sh);
	} else if (role == IC_ROLE_SLAVE) {
		shell_print(sh, "Audio     : %s (configured by master)",
			    le_audio_is_streaming() ? "STREAMING" : "idle");
	}
	shell_print(sh, "Known     : %u helmet(s)", n_peers);
	for (uint8_t i = 0; i < n_peers; i++) {
		bt_addr_le_to_str(&peers[i], addr, sizeof(addr));
		shell_print(sh, "            %s", addr);
	}
	return 0;
}

int intercom_init(void)
{
	bt_le_scan_cb_register(&scan_cb);
	return ic_audio_init();
}
