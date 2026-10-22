/*
 * Helmet - che do Dinh vi (NAV), ban don gian.
 * Thiet ke: docs/HELMET_NAV_DINH_VI.md
 *
 * Muc tieu: xe va hai mu cung bat Dinh vi -> moi mu in log moi 2 s ba khoang cach:
 *   NAV: bike 4.21 m | helmet XX 2.10 m | that helmet -> bike 3.80 m (3 s)
 * Moi so la lan do gan nhat (giu toi HOLD_MS, cu hon AGE_SHOW_MS thi in kem tuoi) nen dong log
 * luon du 3 so khi moi canh da do duoc it nhat mot lan. Mu ban gui so kem tuoi that cua no.
 *
 *   Bat / tat: giu Call 1 s hoac "helmet nav on|off". Tu tat sau NAV_MODE_MIN phut. Bat Dinh vi
 *   thi ket thuc intercom (hai thiet bi chi co mot ket noi voi nhau).
 *   Mu them Service Data Dinh vi {UUID, ver, loai = mu, ma} vao quang ba chinh va quet:
 *     - thay xe -> ket noi toi xe;
 *     - thay mu -> mu co MA NHO HON ket noi, gui IC_OP_NAV_HELLO qua HIS de mu kia nhan ra.
 *   BEN KET NOI (central) LA BEN DO (CS initiator, IPT, nhu mau ipt_initiator cua Nordic): doc kha
 *   nang, tao cau hinh, bat CS Security (chi central duoc bat), bat thu tuc lap, tu tinh khoang
 *   cach bang IFFT. Ben kia chi bat vai tro phan xa.
 *   Hai mu trao doi qua HIS co san (Control Point / Event, [op][tuoi x100 ms][cm]): khoang cach
 *   cua minh toi xe (IC_OP_NAV_DIST_BIKE) va khoang cach mu-mu ben central do (IC_OP_NAV_DIST_PEER).
 *   Lien ket toi xe dung thu tuc ngan hon (36 kenh, subevent <= 2,5 ms) cho de len lich ben canh
 *   cac su kien cua xe; xe tam dung Auracast trong luc Dinh vi (apps/bike nav_hub.c).
 *   Ket noi den trong luc Dinh vi la "ung vien": gui NAV_HELLO -> mu ban; CAND_S giay khong gui
 *   -> giao cho luong dien thoai (nhu ung vien cua intercom).
 *
 * Luong: trang thai che do / lien ket chi doi trong thread bo dieu khien (nav_handle, EVT_NAV).
 * Callback Bluetooth va hen gio chi dua viec vao nav_q. Bo do CS (lenh HCI, IFFT) va gui so lieu
 * qua HIS chay trong system workqueue (bt_rx va sysworkq deu la thread cooperative).
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <bluetooth/cs_de.h>

#include "helmet.h"
#include "nav_proto.h"

LOG_MODULE_REGISTER(nav, LOG_LEVEL_INF);

#define NAV_MODE_MIN    10   /* tu tat che do sau ngan nay phut */
#define NAV_CONN_INT    27   /* x 1,25 ms = 33,75 ms: KHONG la boi cua 10 ms (BIG / CIS) */
#define NAV_SUP_TIMEOUT 400  /* x 10 ms = 4 s */
#define NAV_RSSI_MIN    (-90)
#define TICK_MS         1000
#define REPORT_TICKS    2     /* in dong 3 khoang cach moi 2 s */
#define STATS_REPORTS   5     /* moi 10 s them 1 dong thong ke thu tuc CS */
#define HOLD_MS         30000 /* giu so do gan nhat toi da 30 s (in kem tuoi) */
#define AGE_SHOW_MS     2500  /* so cu hon -> in kem tuoi */
#define SEND_MAX_MS     10000 /* chi gui cho mu ban so do trong 10 s gan nhat */
#define TX_MS           500  /* gui so lieu cho mu ban, xen ke 2 loai */
#define CONNECT_S       10
#define SETUP_S         15   /* ma hoa + do HIS */
#define CAND_S          6    /* ung vien khong gui NAV_HELLO -> giao cho dien thoai */
#define RETRY_MS        3000
#define CS_RETRY_MS     5000
#define MAX_CAND        2
#define MEDIAN_N        5
#define CS_CONFIG_ID    0
#define CS_MODE0_STEPS  3
#define CH_INDEX_OFFSET 2  /* kenh CS 2..76 -> iq[0..74] */
#define CH_COUNT_MAX    79 /* bit 0..78 cua channel map */
#define PROC_MAX_EVENTS 6
#define SUB_MIN_US      1250
#define BIKE_SUB_MAX_US 2500 /* lien ket toi xe: subevent ngan, de chen giua cac su kien cua xe */
#define PEER_SUB_MAX_US 3750
#define PEER_PERIOD_MS  1000 /* 2 thu tuc moi lan in */
#define PERIOD_MIN_MS   200
#define PERIOD_MAX_MS   5000

enum lk { LK_BIKE, LK_PEER, LK_COUNT };

enum lst {
	L_IDLE,       /* chua co, dang quet */
	L_CONNECTING, /* central: dang tao ket noi */
	L_SECURING,   /* da ket noi, dang ma hoa */
	L_HIS,        /* central toi mu ban: do HIS, gui NAV_HELLO */
	L_UP,         /* san sang: central thi bo do CS dang thiet lap / dang do */
};

static const char *const lst_names[] = {"searching", "connecting", "securing", "handshake", "up"};

/* ---------------- Bo do CS (initiator) ---------------- */

enum cs_st {
	CS_OFF,
	CS_CAPS,       /* cai dat mac dinh (initiator) + doc kha nang ben kia */
	CS_WAIT_CAPS,
	CS_CONFIG,     /* tao cau hinh 0 (mode 2, IPT) cho ca hai ben */
	CS_WAIT_CONFIG,
	CS_SEC,        /* bat CS Security (minh la central) */
	CS_WAIT_SEC,
	CS_ENABLE,     /* tham so thu tuc + bat */
	CS_WAIT_ENABLE,
	CS_RUNNING,
	CS_FAILED,
};

static const char *const cs_names[] = {
	"off",         "CS caps",     "CS caps",   "CS config", "CS config", "CS security",
	"CS security", "CS enable",   "CS enable", "measuring", "CS FAILED",
};
BUILD_ASSERT(ARRAY_SIZE(cs_names) == CS_FAILED + 1);

struct cs_eng {
	struct bt_conn *conn; /* giu 1 ref, doi duoi lock */
	atomic_t st;
	struct {
		float i;
		float q;
	} iq[CS_DE_NUM_CHANNELS];
	uint32_t cur_proc;
	uint32_t dropped_proc;
	uint32_t aborted_proc;
	uint32_t n_ok;
	uint32_t n_abort;
	uint32_t n_drop;
	uint32_t n_bad;
	uint8_t last_proc_abort;
	uint8_t last_sub_abort;
	uint8_t n_channels;
	struct k_work dist_work;
	float est[MEDIAN_N];
	uint8_t est_n;
	uint8_t est_idx;
	uint16_t cm; /* trung vi MEDIAN_N thu tuc gan nhat */
	uint8_t q;   /* 0..100 tu do phan tan (MAD) */
	int64_t ts;  /* 0 = chua co */
};

static struct cs_eng eng[LK_COUNT];
static float scratch[2 * CONFIG_BT_CS_DE_NFFT_SIZE]; /* IFFT, dung chung (dist_work tuan tu) */

/* Lien ket toi xe, chinh bang shell (ap dung tu lien ket sau). Moi 2 kenh (36 kenh): thu tuc
 * ngan bang nua, de len lich hon; van trai ~72 MHz nen do phan giai gan nhu khong doi, chi tam
 * khong nhap nhang giam con 75 m.
 */
static uint8_t bike_stride = 2;
static uint16_t bike_period_ms = 500;

/* So do gan nhat cua bo do tung lien ket (giu qua mat lien ket; dist_fn ghi duoi lock) */
static uint16_t last_cm[LK_COUNT] = {UINT16_MAX, UINT16_MAX};
static int64_t last_ts[LK_COUNT];

/* ---------------- Hang doi su kien ---------------- */

enum qev {
	QE_FOUND,        /* a = enum lk, b = RSSI, addr, tag: thiet bi phu hop trong quang ba */
	QE_CONNECTED,    /* a = enum lk, b = loi HCI */
	QE_INCOMING,     /* ket noi den (ung vien mu ban) */
	QE_SECURED,      /* a = 1 neu loi, b = enum bt_security_err */
	QE_DISCONNECTED, /* a = ly do */
	QE_HIS,          /* a = 1 neu loi: do HIS xong (central toi mu ban) */
	QE_HELLO,        /* mu ban gui NAV_HELLO */
	QE_CS_FAIL,      /* a = enum lk */
	QE_TICK,
	QE_CMD,          /* a = 1 bat / 0 tat / 2 dao */
};

struct qitem {
	uint8_t ev;
	uint8_t a;
	uint8_t b;
	uint8_t pad;
	uint32_t tag;
	bt_addr_le_t addr;
	struct bt_conn *conn; /* giu 1 ref, nha sau khi xu ly */
};

K_MSGQ_DEFINE(nav_q, sizeof(struct qitem), 12, 4);

static void q_post(uint8_t ev, uint8_t a, uint8_t b, struct bt_conn *conn,
		   const bt_addr_le_t *addr, uint32_t tag)
{
	struct qitem it = {
		.ev = ev, .a = a, .b = b, .tag = tag, .conn = conn ? bt_conn_ref(conn) : NULL,
	};

	if (addr != NULL) {
		bt_addr_le_copy(&it.addr, addr);
	}
	if (k_msgq_put(&nav_q, &it, K_NO_WAIT) != 0) {
		LOG_WRN("NAV queue full (event %u lost)", ev);
		if (it.conn != NULL) {
			bt_conn_unref(it.conn);
		}
		return;
	}
	app_post(EVT_NAV, 0, 0, 0);
}

static void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	q_post(QE_TICK, 0, 0, NULL, NULL, 0);
}

static K_TIMER_DEFINE(tick_timer, tick_fn, NULL);

/* ---------------- Trang thai dung chung giua cac thread ---------------- */

static struct k_spinlock lock;
static atomic_t active;            /* che do dang bat */
static atomic_t want_lk[LK_COUNT]; /* scan_recv chi bao khi lien ket nay dang can */
static atomic_t found_pending[LK_COUNT];
static atomic_t connecting;        /* dang tao ket noi toi target_addr */
static bt_addr_le_t target_addr;
static uint8_t target_lk;
static uint32_t my_tag;
static struct bt_conn *owned[LK_COUNT + MAX_CAND]; /* ket noi cua nav.c (giu ref) */
static struct bt_conn *tx_conn; /* lien ket mu ban de gui so lieu (giu ref) */
static bool tx_central;         /* minh la central tren lien ket mu ban */

/* So lieu nhan tu mu ban (bt_rx ghi duoi lock) */
static uint16_t rx_peer_cm = UINT16_MAX; /* khoang cach mu-mu do mu ban (central) do */
static int64_t rx_peer_ts;
static uint16_t rx_pb_cm = UINT16_MAX; /* khoang cach mu ban -> xe */
static int64_t rx_pb_ts;

/* Service Data Dinh vi cua mu (main.c dua vao quang ba khi dang Dinh vi) */
uint8_t nav_adv_svc[NAV_ADV_LEN];

/* ---------------- Trang thai chi thread bo dieu khien ---------------- */

struct link {
	struct bt_conn *conn; /* giu ref */
	enum lst st;
	bool central;         /* minh la central = ben do */
	int64_t t_step;
	int64_t retry_after;
};

static struct link lk[LK_COUNT];
static struct bt_conn *cand[MAX_CAND];
static int64_t cand_t[MAX_CAND];
static int64_t t_mode;
static uint32_t n_ticks;
static bool scanning;

/* ---------------- Tien ich ---------------- */

static void cm_str(uint16_t cm, char *buf, size_t len)
{
	if (cm == UINT16_MAX) {
		snprintk(buf, len, "-");
	} else {
		snprintk(buf, len, "%u.%02u m", cm / 100U, cm % 100U);
	}
}

static const char *conn_str(const struct bt_conn *conn, char *buf, size_t len)
{
	bt_addr_le_to_str(bt_conn_get_dst(conn), buf, len);
	return buf;
}

static bool owned_has(const struct bt_conn *conn)
{
	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] != NULL && owned[i] == conn) {
			return true;
		}
	}
	return false;
}

static void own_add(struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] == NULL) {
			owned[i] = bt_conn_ref(conn);
			break;
		}
	}
	k_spin_unlock(&lock, key);
}

static void own_remove(const struct bt_conn *conn)
{
	struct bt_conn *found = NULL;
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (size_t i = 0; i < ARRAY_SIZE(owned); i++) {
		if (owned[i] != NULL && owned[i] == conn) {
			found = owned[i];
			owned[i] = NULL;
			break;
		}
	}
	k_spin_unlock(&lock, key);
	if (found != NULL) {
		bt_conn_unref(found);
	}
}

static int lk_of(const struct bt_conn *conn)
{
	for (int i = 0; i < LK_COUNT; i++) {
		if (lk[i].conn != NULL && lk[i].conn == conn) {
			return i;
		}
	}
	return -1;
}

static int cand_of(const struct bt_conn *conn)
{
	for (int i = 0; i < MAX_CAND; i++) {
		if (cand[i] != NULL && cand[i] == conn) {
			return i;
		}
	}
	return -1;
}

static const char *lk_name(int i)
{
	return i == LK_BIKE ? "bike" : "peer helmet";
}

/* So do gan nhat cua lien ket i neu khong cu hon max_ms; *age = tuoi (ms) */
static uint16_t own_cm(int i, int64_t now, int64_t max_ms, int64_t *age)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint16_t cm = UINT16_MAX;

	if (last_cm[i] != UINT16_MAX && now - last_ts[i] <= max_ms) {
		cm = last_cm[i];
		*age = now - last_ts[i];
	}
	k_spin_unlock(&lock, key);
	return cm;
}

/* ---------------- Bo do CS (system workqueue) ---------------- */

static void cs_step_fn(struct k_work *w);
static K_WORK_DEFINE(cs_step_work, cs_step_fn);

static int eng_idx(const struct bt_conn *conn)
{
	for (int i = 0; i < LK_COUNT; i++) {
		if (eng[i].conn != NULL && eng[i].conn == conn) {
			return i;
		}
	}
	return -1;
}

static void eng_fail(int i, const char *what, int err)
{
	atomic_set(&eng[i].st, CS_FAILED);
	LOG_WRN("NAV %s: %s failed (%d) - reconnecting in %d s", lk_name(i), what, err,
		CS_RETRY_MS / 1000);
	q_post(QE_CS_FAIL, (uint8_t)i, 0, NULL, NULL, 0);
}

static void eng_advance(int i, enum cs_st from, enum cs_st to)
{
	if (atomic_cas(&eng[i].st, from, to)) {
		k_work_submit(&cs_step_work);
	}
}

/* Thread bo dieu khien: bat bo do tren lien ket ma minh la central */
static void eng_start(int i, struct bt_conn *conn)
{
	struct cs_eng *e = &eng[i];
	struct bt_conn *old;
	k_spinlock_key_t key = k_spin_lock(&lock);

	old = e->conn;
	e->conn = bt_conn_ref(conn);
	e->n_ok = e->n_abort = e->n_drop = e->n_bad = 0;
	e->last_proc_abort = e->last_sub_abort = 0;
	e->cur_proc = e->dropped_proc = e->aborted_proc = UINT32_MAX;
	e->est_n = e->est_idx = 0;
	e->ts = 0;
	e->cm = UINT16_MAX;
	atomic_set(&e->st, CS_CAPS);
	k_spin_unlock(&lock, key);
	if (old != NULL) {
		bt_conn_unref(old);
	}
	k_work_submit(&cs_step_work);
}

static void eng_stop(int i)
{
	struct bt_conn *old;
	k_spinlock_key_t key = k_spin_lock(&lock);

	old = eng[i].conn;
	eng[i].conn = NULL;
	eng[i].ts = 0;
	atomic_set(&eng[i].st, CS_OFF);
	k_spin_unlock(&lock, key);
	if (old != NULL) {
		bt_conn_unref(old);
	}
}

static void config_get(int i, struct cs_eng *e, struct bt_le_cs_create_config_params *p)
{
	const uint8_t stride = (i == LK_BIKE) ? bike_stride : 1;

	memset(p, 0, sizeof(*p));
	p->id = CS_CONFIG_ID;
	p->mode = BT_CONN_LE_CS_MAIN_MODE_2_NO_SUB_MODE;
	p->min_main_mode_steps = 2;
	p->max_main_mode_steps = 5;
	p->main_mode_repetition = 0;
	p->mode_0_steps = CS_MODE0_STEPS;
	p->role = BT_CONN_LE_CS_ROLE_INITIATOR;
	p->rtt_type = BT_CONN_LE_CS_RTT_TYPE_AA_ONLY;
	p->cs_sync_phy = BT_CONN_LE_CS_SYNC_1M_PHY;
	p->channel_map_repetition = 1;
	p->channel_selection_type = BT_CONN_LE_CS_CHSEL_TYPE_3B;
	p->ch3c_shape = BT_CONN_LE_CS_CH3C_SHAPE_HAT;
	p->ch3c_jump = 2;
	p->cs_enhancements_1 = 1; /* Inline PCT Transfer */

	bt_le_cs_set_valid_chmap_bits(p->channel_map);
	e->n_channels = 0;
	for (uint8_t ch = 0; ch < CH_COUNT_MAX; ch++) {
		if (stride > 1 && (ch % stride) != 0) {
			p->channel_map[ch / 8] &= ~BIT(ch % 8);
		}
		if (p->channel_map[ch / 8] & BIT(ch % 8)) {
			e->n_channels++;
		}
	}
}

static int set_procedure_params(int i, struct bt_conn *c)
{
	struct bt_conn_info info;
	uint32_t int_us = 33750U;
	uint16_t acl_units;
	uint16_t interval_ev;

	if (bt_conn_get_info(c, &info) == 0 && info.le.interval_us > 0) {
		int_us = info.le.interval_us;
	}
	acl_units = (uint16_t)(int_us / 625U);
	interval_ev = (uint16_t)MAX(2U, DIV_ROUND_CLOSEST((i == LK_BIKE ? bike_period_ms
								       : PEER_PERIOD_MS) * 1000U,
							   int_us));

	const struct bt_le_cs_set_procedure_parameters_param p = {
		.config_id = CS_CONFIG_ID,
		.max_procedure_len = acl_units * MIN(PROC_MAX_EVENTS, interval_ev - 1),
		.min_procedure_interval = interval_ev,
		.max_procedure_interval = interval_ev,
		.max_procedure_count = 0, /* lap toi khi ngat */
		.min_subevent_len = SUB_MIN_US,
		.max_subevent_len = (i == LK_BIKE) ? BIKE_SUB_MAX_US : PEER_SUB_MAX_US,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80, /* khong yeu cau */
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	LOG_INF("NAV %s: ACL %u.%02u ms -> 1 procedure every %u events (~%u ms), %u channels",
		lk_name(i), int_us / 1000U, (int_us % 1000U) / 10U, interval_ev,
		interval_ev * int_us / 1000U, eng[i].n_channels);
	return bt_le_cs_set_procedure_parameters(c, &p);
}

static void eng_step(int i, struct bt_conn *c)
{
	const struct bt_le_cs_set_default_settings_param ds = {
		.enable_initiator_role = true,
		.enable_reflector_role = false,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};
	struct bt_le_cs_create_config_params cfg;
	int err;

	switch ((enum cs_st)atomic_get(&eng[i].st)) {
	case CS_CAPS:
		err = bt_le_cs_set_default_settings(c, &ds);
		if (err == 0) {
			atomic_set(&eng[i].st, CS_WAIT_CAPS);
			err = bt_le_cs_read_remote_supported_capabilities(c);
		}
		if (err) {
			eng_fail(i, "capability exchange", err);
		}
		break;

	case CS_CONFIG:
		config_get(i, &eng[i], &cfg);
		atomic_set(&eng[i].st, CS_WAIT_CONFIG);
		err = bt_le_cs_create_config(c, &cfg, BT_LE_CS_CREATE_CONFIG_CONTEXT_LOCAL_AND_REMOTE);
		if (err) {
			eng_fail(i, "config create", err);
		}
		break;

	case CS_SEC:
		atomic_set(&eng[i].st, CS_WAIT_SEC);
		err = bt_le_cs_security_enable(c);
		if (err) {
			eng_fail(i, "CS security", err);
		}
		break;

	case CS_ENABLE: {
		const struct bt_le_cs_procedure_enable_param ep = {
			.config_id = CS_CONFIG_ID,
			.enable = BT_CONN_LE_CS_PROCEDURES_ENABLED,
		};

		atomic_set(&eng[i].st, CS_WAIT_ENABLE);
		err = set_procedure_params(i, c);
		if (err == 0) {
			err = bt_le_cs_procedure_enable(c, &ep);
		}
		if (err) {
			eng_fail(i, "procedure enable", err);
		}
		break;
	}

	default:
		break;
	}
}

static void cs_step_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	for (int i = 0; i < LK_COUNT; i++) {
		struct bt_conn *c;
		k_spinlock_key_t key = k_spin_lock(&lock);

		c = eng[i].conn != NULL ? bt_conn_ref(eng[i].conn) : NULL;
		k_spin_unlock(&lock, key);
		if (c != NULL) {
			eng_step(i, c);
			bt_conn_unref(c);
		}
	}
}

static int float_cmp(const void *a, const void *b)
{
	const float fa = *(const float *)a;
	const float fb = *(const float *)b;

	return (fa > fb) - (fa < fb);
}

static float median(float *v, uint8_t n)
{
	qsort(v, n, sizeof(float), float_cmp);
	return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

static void dist_fn(struct k_work *w)
{
	struct cs_eng *e = CONTAINER_OF(w, struct cs_eng, dist_work);
	float tmp[MEDIAN_N];
	float med;
	float mad;
	float d;
	k_spinlock_key_t key;

	/* IQ cua thu tuc vua xong vao bo nho tam chung, phan con lai = 0 (dem IFFT) */
	memset(scratch, 0, sizeof(scratch));
	memcpy(scratch, e->iq, sizeof(e->iq));
	d = cs_de_ifft(scratch);
	if (!isfinite(d) || d < 0.0f) {
		e->n_bad++;
		return;
	}
	e->est[e->est_idx] = d;
	e->est_idx = (e->est_idx + 1) % MEDIAN_N;
	if (e->est_n < MEDIAN_N) {
		e->est_n++;
	}
	/* Trung vi + do lech tuyet doi trung vi (MAD, m): MAD 0 -> chat luong 100, >= 1 m -> 0 */
	memcpy(tmp, e->est, e->est_n * sizeof(float));
	med = median(tmp, e->est_n);
	for (uint8_t i = 0; i < e->est_n; i++) {
		tmp[i] = fabsf(e->est[i] - med);
	}
	mad = median(tmp, e->est_n);

	key = k_spin_lock(&lock);
	e->cm = (uint16_t)MIN(lroundf(med * 100.0f), UINT16_MAX - 1);
	e->q = (uint8_t)CLAMP(lroundf(100.0f * (1.0f - mad)), 0, 100);
	if (e->est_n < 3) {
		e->q = MIN(e->q, 40);
	}
	e->ts = k_uptime_get();
	last_cm[e - eng] = e->cm;
	last_ts[e - eng] = e->ts;
	k_spin_unlock(&lock, key);
}

/* ---------------- Callback CS (bt_rx) ---------------- */

static void remote_caps_cb(struct bt_conn *conn, uint8_t status,
			   struct bt_conn_le_cs_capabilities *p)
{
	const int i = eng_idx(conn);

	if (i < 0 || atomic_get(&eng[i].st) != CS_WAIT_CAPS) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		eng_fail(i, "capability exchange", -status);
		return;
	}
	/* Nhu mau ipt_initiator: chi can ben kia phan xa duoc. t_ip2_ipt chi co trong su kien v2
	 * (Zephyr dat 0 neu controller gui v1) nen khong dung de chan; ben kia khong ho tro IPT
	 * thi buoc tao cau hinh se bao loi.
	 */
	LOG_INF("NAV %s: peer CS reflector %s (IPT t_ip2 0x%04x, t_sw %u us)", lk_name(i),
		p->reflector_supported ? "yes" : "NO", p->t_ip2_ipt_times_supported,
		p->t_sw_ipt_time_supported);
	if (!p->reflector_supported) {
		eng_fail(i, "peer without CS reflector", -ENOTSUP);
		return;
	}
	eng_advance(i, CS_WAIT_CAPS, CS_CONFIG);
}

static void config_cb(struct bt_conn *conn, uint8_t status, struct bt_conn_le_cs_config *cfg)
{
	const int i = eng_idx(conn);

	if (i < 0 || atomic_get(&eng[i].st) != CS_WAIT_CONFIG) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		eng_fail(i, "config create (IPT)", -status);
		return;
	}
	LOG_INF("NAV %s: CS config %u (mode %u, T_IP2 %u us, T_FCS %u us, T_PM %u us)", lk_name(i),
		cfg->id, cfg->mode, cfg->t_ip2_time_us, cfg->t_fcs_time_us, cfg->t_pm_time_us);
	eng_advance(i, CS_WAIT_CONFIG, CS_SEC);
}

static void security_cb(struct bt_conn *conn, uint8_t status)
{
	const int i = eng_idx(conn);

	if (i < 0 || atomic_get(&eng[i].st) != CS_WAIT_SEC) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		eng_fail(i, "CS security", -status);
		return;
	}
	eng_advance(i, CS_WAIT_SEC, CS_ENABLE);
}

static void proc_enable_cb(struct bt_conn *conn, uint8_t status,
			   struct bt_conn_le_cs_procedure_enable_complete *p)
{
	const int i = eng_idx(conn);

	if (i < 0) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		eng_fail(i, "procedure enable", -status);
		return;
	}
	if (p->state != BT_CONN_LE_CS_PROCEDURES_ENABLED) {
		return;
	}
	LOG_INF("NAV %s: MEASURING - subevent %u us x %u/event, procedure every %u conn events, "
		"TX %d dBm",
		lk_name(i), p->subevent_len, p->subevents_per_event, p->procedure_interval,
		p->selected_tx_power);
	eng_advance(i, CS_WAIT_ENABLE, CS_RUNNING);
}

static void steps_parse(struct cs_eng *e, struct bt_conn_le_cs_subevent_result *r)
{
	for (uint8_t i = 0; i < r->header.num_steps_reported; i++) {
		struct bt_le_cs_subevent_step step;

		if (r->step_data_buf->len < 3) {
			return;
		}
		step.mode = net_buf_simple_pull_u8(r->step_data_buf);
		step.channel = net_buf_simple_pull_u8(r->step_data_buf);
		step.data_len = net_buf_simple_pull_u8(r->step_data_buf);
		if (step.data_len == 0 || step.data_len > r->step_data_buf->len) {
			return;
		}
		step.data = r->step_data_buf->data;

		if (step.mode == BT_HCI_OP_LE_CS_MAIN_MODE_2 && step.channel >= CH_INDEX_OFFSET &&
		    step.channel < CH_INDEX_OFFSET + CS_DE_NUM_CHANNELS) {
			const struct bt_hci_le_cs_step_data_mode_2 *m2 = (const void *)step.data;
			const struct bt_le_cs_iq_sample s =
				bt_le_cs_parse_pct(m2->tone_info[0].phase_correction_term);

			e->iq[step.channel - CH_INDEX_OFFSET].i = s.i;
			e->iq[step.channel - CH_INDEX_OFFSET].q = s.q;
		}
		net_buf_simple_pull(r->step_data_buf, step.data_len);
	}
}

static void subevent_cb(struct bt_conn *conn, struct bt_conn_le_cs_subevent_result *r)
{
	const int i = eng_idx(conn);
	struct cs_eng *e;
	uint16_t pc;

	if (i < 0 || atomic_get(&eng[i].st) != CS_RUNNING) {
		return;
	}
	e = &eng[i];
	pc = r->header.procedure_counter;
	if (r->header.procedure_abort_reason != BT_HCI_LE_CS_PROCEDURE_ABORT_REASON_NO_ABORT ||
	    r->header.subevent_abort_reason != BT_HCI_LE_CS_SUBEVENT_ABORT_REASON_NO_ABORT) {
		if (e->aborted_proc != pc) {
			e->aborted_proc = pc;
			e->n_abort++;
			e->last_proc_abort = r->header.procedure_abort_reason;
			e->last_sub_abort = r->header.subevent_abort_reason;
		}
		return;
	}
	if (pc == e->dropped_proc) {
		return;
	}
	if (k_work_is_pending(&e->dist_work)) {
		e->dropped_proc = pc; /* thu tuc truoc chua tinh xong (dung chung iq) */
		e->n_drop++;
		return;
	}
	if (pc != e->cur_proc) {
		memset(e->iq, 0, sizeof(e->iq));
		e->cur_proc = pc;
	}
	steps_parse(e, r);
	if (r->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
		e->n_ok++;
		k_work_submit(&e->dist_work);
	}
}

BT_CONN_CB_DEFINE(nav_cs_cb) = {
	.le_cs_read_remote_capabilities_complete = remote_caps_cb,
	.le_cs_config_complete = config_cb,
	.le_cs_security_enable_complete = security_cb,
	.le_cs_procedure_enable_complete = proc_enable_cb,
	.le_cs_subevent_data_available = subevent_cb,
};

/* ---------------- Gui so lieu cho mu ban (system workqueue) ---------------- */

static void tx_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(tx_work, tx_fn);

static void tx_fn(struct k_work *w)
{
	static bool send_peer;
	const int64_t now = k_uptime_get();
	struct bt_conn *c;
	bool central;
	int64_t age = 0;
	uint16_t cm;
	uint8_t op;
	k_spinlock_key_t key;

	ARG_UNUSED(w);
	if (!atomic_get(&active)) {
		return;
	}
	k_work_reschedule(&tx_work, K_MSEC(TX_MS));

	key = k_spin_lock(&lock);
	c = tx_conn != NULL ? bt_conn_ref(tx_conn) : NULL;
	central = tx_central;
	k_spin_unlock(&lock, key);
	if (c == NULL) {
		return;
	}
	/* Xen ke: khoang cach minh -> xe; khoang cach mu-mu (chi ben central do). Kem tuoi that cua
	 * so do de mu ban in dung (khong phai luc nhan).
	 */
	send_peer = !send_peer && central;
	op = send_peer ? IC_OP_NAV_DIST_PEER : IC_OP_NAV_DIST_BIKE;
	cm = own_cm(send_peer ? LK_PEER : LK_BIKE, now, SEND_MAX_MS, &age);
	if (cm != UINT16_MAX && !ic_svc_busy()) {
		(void)ic_svc_send3(c, central, op, (uint8_t)MIN(age / 100, 254), cm & 0xFF, cm >> 8);
	}
	bt_conn_unref(c);
}

static void tx_set(struct bt_conn *conn, bool central)
{
	struct bt_conn *old;
	k_spinlock_key_t key = k_spin_lock(&lock);

	old = tx_conn;
	tx_conn = conn != NULL ? bt_conn_ref(conn) : NULL;
	tx_central = central;
	k_spin_unlock(&lock, key);
	if (old != NULL) {
		bt_conn_unref(old);
	}
}

/* ---------------- Quet ---------------- */

struct adv_hit {
	bool ok;
	enum nav_dev_type type;
	uint32_t tag;
};

static bool ad_cb(struct bt_data *data, void *user_data)
{
	struct adv_hit *h = user_data;

	if (data->type == BT_DATA_SVC_DATA128 &&
	    nav_adv_parse(data->data, data->data_len, &h->type, &h->tag)) {
		h->ok = true;
		return false;
	}
	return true;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	struct net_buf_simple_state bs;
	struct adv_hit h = {0};
	bt_addr_le_t addr;
	int i;

	if (!atomic_get(&active) || info->rssi < NAV_RSSI_MIN ||
	    (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) == 0) {
		return;
	}
	net_buf_simple_save(buf, &bs);
	bt_data_parse(buf, ad_cb, &h);
	net_buf_simple_restore(buf, &bs);
	if (!h.ok) {
		return;
	}
	if (h.type == NAV_TYPE_BIKE) {
		i = LK_BIKE;
	} else if (h.type == NAV_TYPE_HELMET && my_tag < h.tag) {
		i = LK_PEER; /* ma nho hon ket noi; mu kia (ma lon hon) cho minh */
	} else {
		return;
	}
	if (!atomic_get(&want_lk[i]) || !atomic_cas(&found_pending[i], 0, 1)) {
		return;
	}
	nav_addr_norm(&addr, info->addr);
	q_post(QE_FOUND, (uint8_t)i, (uint8_t)info->rssi, NULL, &addr, h.tag);
}

static struct bt_le_scan_cb scan_cb = {
	.recv = scan_recv,
};

static void scan_update(void)
{
	/* 30 ms / 90 ms: bat quang ba 100-150 ms trong ~0,5 s, it ton pin hon quet lien tuc */
	static const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0090,
		.window = 0x0030,
	};
	const int64_t now = k_uptime_get();
	bool need = false;

	for (int i = 0; i < LK_COUNT; i++) {
		const bool w = atomic_get(&active) && lk[i].st == L_IDLE && now >= lk[i].retry_after;

		atomic_set(&want_lk[i], w);
		need |= w;
	}
	/* Chi mot lenh tao ket noi mot luc: dang tao thi khong quet */
	if (atomic_get(&connecting)) {
		need = false;
	}
	if (need && !scanning) {
		const int err = bt_le_scan_start(&param, NULL);

		scanning = (err == 0 || err == -EALREADY);
	} else if (!need && scanning) {
		(void)bt_le_scan_stop();
		scanning = false;
	}
}

/* ---------------- Lien ket ---------------- */

static void link_drop(int i, uint32_t retry_ms)
{
	struct link *l = &lk[i];

	eng_stop(i);
	if (i == LK_PEER) {
		tx_set(NULL, false);
	}
	if (l->conn != NULL) {
		(void)bt_conn_disconnect(l->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(l->conn);
		l->conn = NULL;
	}
	if (l->st == L_CONNECTING) {
		atomic_clear(&connecting);
	}
	l->st = L_IDLE;
	l->central = false;
	l->retry_after = k_uptime_get() + retry_ms;
	atomic_clear(&found_pending[i]);
}

static void cand_release(int i, bool to_phone)
{
	struct bt_conn *c = cand[i];

	cand[i] = NULL;
	if (c == NULL) {
		return;
	}
	own_remove(c);
	if (to_phone) {
		app_phone_adopt(c); /* chuyen luon ref */
	} else {
		(void)bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(c);
	}
}

static void on_found(const struct qitem *it)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const int i = it->a;
	struct link *l = &lk[i];
	struct bt_conn *c = NULL;
	k_spinlock_key_t key;
	int err;

	atomic_clear(&found_pending[i]);
	if (!atomic_get(&active) || l->st != L_IDLE || atomic_get(&connecting)) {
		return;
	}
	if (scanning) {
		(void)bt_le_scan_stop();
		scanning = false;
	}
	key = k_spin_lock(&lock);
	bt_addr_le_copy(&target_addr, &it->addr);
	target_lk = (uint8_t)i;
	k_spin_unlock(&lock, key);
	atomic_set(&connecting, 1);
	err = bt_conn_le_create(&it->addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM(NAV_CONN_INT, NAV_CONN_INT, 0, NAV_SUP_TIMEOUT), &c);
	bt_addr_le_to_str(&it->addr, addr, sizeof(addr));
	if (err) {
		LOG_WRN("NAV %s: connect to %s failed (%d) - retrying", lk_name(i), addr, err);
		atomic_clear(&connecting);
		l->retry_after = k_uptime_get() + RETRY_MS;
		return;
	}
	l->conn = c;
	l->central = true;
	l->st = L_CONNECTING;
	l->t_step = k_uptime_get();
	if (i == LK_BIKE) {
		LOG_INF("NAV bike: %s found (RSSI %d dBm) - connecting", addr, (int8_t)it->b);
	} else {
		LOG_INF("NAV peer helmet: %s found (RSSI %d dBm, tag %08x > ours %08x) - connecting",
			addr, (int8_t)it->b, it->tag, my_tag);
	}
}

static void on_connected(const struct qitem *it)
{
	const int i = it->a;
	struct link *l = &lk[i];
	int err;

	if (l->st != L_CONNECTING || l->conn != it->conn) {
		return;
	}
	if (it->b) {
		LOG_WRN("NAV %s: connection failed (0x%02x %s)", lk_name(i), it->b,
			bt_hci_err_to_str(it->b));
		link_drop(i, RETRY_MS);
		return;
	}
	err = bt_conn_set_security(l->conn, BT_SECURITY_L2);
	if (err) {
		LOG_WRN("NAV %s: security request failed (%d)", lk_name(i), err);
		link_drop(i, RETRY_MS);
		return;
	}
	l->st = L_SECURING;
	l->t_step = k_uptime_get();
}

static void on_incoming(const struct qitem *it)
{
	const struct bt_le_cs_set_default_settings_param ds = {
		.enable_initiator_role = false,
		.enable_reflector_role = true,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};
	char addr[BT_ADDR_LE_STR_LEN];
	int i;

	for (i = 0; i < MAX_CAND && cand[i] != NULL; i++) {
	}
	if (!atomic_get(&active) || lk[LK_PEER].st != L_IDLE || i == MAX_CAND) {
		/* Da co mu ban (hoac het cho): ket noi nay la dien thoai */
		own_remove(it->conn);
		app_phone_adopt(bt_conn_ref(it->conn));
		return;
	}
	cand[i] = bt_conn_ref(it->conn);
	cand_t[i] = k_uptime_get();
	/* San sang phan xa truoc khi mu ban (central) tao cau hinh CS */
	(void)bt_le_cs_set_default_settings(cand[i], &ds);
	(void)bt_conn_set_security(cand[i], BT_SECURITY_L2);
	LOG_INF("NAV: incoming connection %s (waiting for NAV_HELLO)",
		conn_str(cand[i], addr, sizeof(addr)));
}

static void on_secured(const struct qitem *it)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const int i = lk_of(it->conn);
	const int ci = cand_of(it->conn);
	int err;

	if (ci >= 0) {
		if (it->a) {
			cand_release(ci, false);
		}
		return; /* cho NAV_HELLO */
	}
	if (i < 0 || lk[i].st != L_SECURING) {
		return;
	}
	if (it->a) {
		/* Khoa cu (ben kia da xoa ghep doi): xoa ben minh, lan sau ghep moi Just Works */
		if (it->b == BT_SECURITY_ERR_PIN_OR_KEY_MISSING ||
		    it->b == BT_SECURITY_ERR_AUTH_FAIL) {
			(void)bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(lk[i].conn));
		}
		LOG_WRN("NAV %s: security failed (%d %s)", lk_name(i), it->b,
			bt_security_err_to_str(it->b));
		link_drop(i, RETRY_MS);
		return;
	}
	if (i == LK_BIKE) {
		lk[i].st = L_UP;
		LOG_INF("NAV bike: linked %s - this helmet measures (CS initiator)",
			conn_str(lk[i].conn, addr, sizeof(addr)));
		eng_start(i, lk[i].conn);
		return;
	}
	/* Mu ban, minh la central: do HIS roi gui NAV_HELLO */
	err = ic_svc_discover(lk[i].conn);
	if (err) {
		LOG_WRN("NAV peer helmet: HIS discovery failed (%d)", err);
		link_drop(i, RETRY_MS);
		return;
	}
	lk[i].st = L_HIS;
	lk[i].t_step = k_uptime_get();
}

static void on_his(const struct qitem *it)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct link *l = &lk[LK_PEER];
	int err;

	if (l->st != L_HIS || l->conn != it->conn) {
		return;
	}
	if (it->a) {
		LOG_WRN("NAV peer helmet: no Helmet Intercom Service");
		link_drop(LK_PEER, RETRY_MS);
		return;
	}
	err = ic_svc_send(l->conn, true, IC_OP_NAV_HELLO, NAV_PROTO_VER);
	if (err) {
		LOG_WRN("NAV peer helmet: NAV_HELLO send failed (%d)", err);
		link_drop(LK_PEER, RETRY_MS);
		return;
	}
	l->st = L_UP;
	tx_set(l->conn, true);
	LOG_INF("NAV peer helmet: linked %s - this helmet measures (CS initiator)",
		conn_str(l->conn, addr, sizeof(addr)));
	eng_start(LK_PEER, l->conn);
}

static void on_hello(const struct qitem *it)
{
	char addr[BT_ADDR_LE_STR_LEN];
	const int ci = cand_of(it->conn);
	struct link *l = &lk[LK_PEER];

	if (ci < 0 || l->st != L_IDLE) {
		return;
	}
	l->conn = cand[ci]; /* chuyen ref */
	cand[ci] = NULL;
	l->central = false;
	l->st = L_UP;
	tx_set(l->conn, false);
	LOG_INF("NAV peer helmet: linked %s - the other helmet measures (this one reflects)",
		conn_str(l->conn, addr, sizeof(addr)));
	/* Ung vien khac (neu co) la dien thoai */
	for (int i = 0; i < MAX_CAND; i++) {
		cand_release(i, true);
	}
}

static void on_disconnected(const struct qitem *it)
{
	const int i = lk_of(it->conn);
	const int ci = cand_of(it->conn);

	if (ci >= 0) {
		struct bt_conn *c = cand[ci];

		cand[ci] = NULL;
		bt_conn_unref(c);
		return;
	}
	if (i < 0) {
		return;
	}
	if (atomic_get(&active)) {
		LOG_WRN("NAV %s: link lost (0x%02x %s) - searching again", lk_name(i), it->a,
			bt_hci_err_to_str(it->a));
	}
	link_drop(i, 1000);
}

/* ---------------- Bat / tat, nhip 1 s ---------------- */

/* "4.21 m" hoac "4.21 m (8 s)" neu cu hon AGE_SHOW_MS */
static void val_str(uint16_t cm, int64_t age, char *buf, size_t len)
{
	if (age > AGE_SHOW_MS) {
		snprintk(buf, len, "%u.%02u m (%d s)", cm / 100U, cm % 100U,
			 (int)MIN((age + 500) / 1000, 999));
	} else {
		cm_str(cm, buf, len);
	}
}

/* So nhan tu mu ban (rx_*), giu toi HOLD_MS */
static uint16_t rx_cm(bool peer, int64_t now, int64_t *age)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint16_t v = peer ? rx_peer_cm : rx_pb_cm;
	const int64_t ts = peer ? rx_peer_ts : rx_pb_ts;
	uint16_t cm = UINT16_MAX;

	if (v != UINT16_MAX && now - ts <= HOLD_MS) {
		cm = v;
		*age = now - ts;
	}
	k_spin_unlock(&lock, key);
	return cm;
}

/* Mot canh: so gan nhat (kem tuoi neu cu), chua tung co thi in trang thai */
static bool link_text(int i, int64_t now, char *buf, size_t len)
{
	const struct link *l = &lk[i];
	int64_t age = 0;
	uint16_t cm;

	if (i == LK_PEER && !l->central && l->st == L_UP) {
		cm = rx_cm(true, now, &age); /* mu ban do canh mu-mu */
	} else {
		cm = own_cm(i, now, HOLD_MS, &age);
		if (cm == UINT16_MAX && i == LK_PEER) {
			cm = rx_cm(true, now, &age); /* lan truoc mu ban la ben do */
		}
	}
	if (cm != UINT16_MAX) {
		val_str(cm, age, buf, len);
		return true;
	}
	if (l->st != L_UP) {
		snprintk(buf, len, "(%s)", lst_names[l->st]);
	} else if (l->central) {
		snprintk(buf, len, "(%s)", cs_names[atomic_get(&eng[i].st)]);
	} else {
		snprintk(buf, len, "(waiting)");
	}
	return false;
}

static void log_line(void)
{
	const int64_t now = k_uptime_get();
	char d_bike[28];
	char d_peer[28];
	char d_pb[28];
	char addr[BT_ADDR_LE_STR_LEN] = "-";
	int64_t age = 0;
	uint16_t pb;

	(void)link_text(LK_BIKE, now, d_bike, sizeof(d_bike));
	(void)link_text(LK_PEER, now, d_peer, sizeof(d_peer));
	pb = rx_cm(false, now, &age);
	if (pb != UINT16_MAX) {
		val_str(pb, age, d_pb, sizeof(d_pb));
	} else {
		snprintk(d_pb, sizeof(d_pb), "(waiting)");
	}
	if (lk[LK_PEER].conn != NULL) {
		conn_str(lk[LK_PEER].conn, addr, sizeof(addr));
	}
	LOG_INF("NAV: bike %s | helmet %s %s | that helmet -> bike %s", d_bike, addr, d_peer, d_pb);
}

/* Thong ke thu tuc CS cua cac lien ket minh do: de biet canh nao bi huy nhieu */
static void log_stats(void)
{
	for (int i = 0; i < LK_COUNT; i++) {
		const struct cs_eng *e = &eng[i];

		if (!lk[i].central || lk[i].st != L_UP) {
			continue;
		}
		LOG_INF("nav: %s link %s, %u procedures ok, %u aborted (last proc %u / subevent %u), "
			"%u dropped, %u invalid, %u channels",
			lk_name(i), cs_names[atomic_get(&e->st)], e->n_ok, e->n_abort,
			e->last_proc_abort, e->last_sub_abort, e->n_drop, e->n_bad, e->n_channels);
	}
}

static void mode_on(void)
{
	const enum helmet_state hs = app_state();
	bt_addr_le_t ids[CONFIG_BT_ID_MAX];
	size_t n = ARRAY_SIZE(ids);

	if (atomic_get(&active)) {
		return;
	}
	if (hs == HS_RINGING || hs == HS_IN_CALL || hs == HS_SHUTDOWN || hs == HS_BOOT) {
		LOG_WRN("Navigation mode not available in state %s", app_state_str(hs));
		return;
	}
	if (intercom_mode() != ICM_NORMAL) {
		LOG_INF("Navigation mode: ending intercom first (one connection per pair of devices)");
		intercom_stop();
	}
	bt_id_get(ids, &n);
	my_tag = n > 0 ? nav_tag_of(&ids[0]) : 0;
	nav_adv_build(nav_adv_svc, NAV_TYPE_HELMET, my_tag);
	for (int i = 0; i < LK_COUNT; i++) {
		lk[i].st = L_IDLE;
		lk[i].central = false;
		lk[i].retry_after = 0;
		last_cm[i] = UINT16_MAX;
	}
	rx_peer_cm = UINT16_MAX;
	rx_pb_cm = UINT16_MAX;
	n_ticks = 0;
	t_mode = k_uptime_get();
	atomic_set(&active, 1);
	app_adv_refresh(); /* quang ba co them Service Data Dinh vi */
	scan_update();
	k_timer_start(&tick_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));
	k_work_reschedule(&tx_work, K_MSEC(TX_MS));
	audio_out_prompt(PROMPT_NAV_ON);
	LOG_INF("Navigation mode ON (%d min), tag %08x - looking for the bike and the other helmet",
		NAV_MODE_MIN, my_tag);
}

static void mode_off(void)
{
	if (!atomic_get(&active)) {
		return;
	}
	atomic_set(&active, 0);
	k_timer_stop(&tick_timer);
	(void)k_work_cancel_delayable(&tx_work);
	for (int i = 0; i < LK_COUNT; i++) {
		link_drop(i, 0);
	}
	for (int i = 0; i < MAX_CAND; i++) {
		cand_release(i, true);
	}
	scan_update();
	app_adv_refresh();
	audio_out_prompt(PROMPT_NAV_OFF);
	LOG_INF("Navigation mode OFF");
	if (app_state() == HS_IDLE) {
		power_activity(); /* hen gio tu tat chay lai tu dau */
	}
}

static void on_tick(void)
{
	const int64_t now = k_uptime_get();

	if (!atomic_get(&active)) {
		return;
	}
	if (now - t_mode > (int64_t)NAV_MODE_MIN * 60 * 1000) {
		LOG_INF("Navigation mode: %d min elapsed", NAV_MODE_MIN);
		mode_off();
		return;
	}
	for (int i = 0; i < MAX_CAND; i++) {
		if (cand[i] != NULL && now - cand_t[i] > CAND_S * 1000) {
			char addr[BT_ADDR_LE_STR_LEN];

			LOG_INF("NAV: %s sent no NAV_HELLO - handing over to phone handling",
				conn_str(cand[i], addr, sizeof(addr)));
			cand_release(i, true);
		}
	}
	for (int i = 0; i < LK_COUNT; i++) {
		const struct link *l = &lk[i];

		if ((l->st == L_CONNECTING && now - l->t_step > CONNECT_S * 1000) ||
		    ((l->st == L_SECURING || l->st == L_HIS) && now - l->t_step > SETUP_S * 1000)) {
			LOG_WRN("NAV %s: %s took too long", lk_name(i), lst_names[l->st]);
			link_drop(i, RETRY_MS);
		}
	}
	if (++n_ticks % REPORT_TICKS == 0) {
		log_line();
		if ((n_ticks / REPORT_TICKS) % STATS_REPORTS == 0) {
			log_stats();
		}
	}
}

/* ---------------- API: thread bo dieu khien ---------------- */

void nav_handle(void)
{
	struct qitem it;

	while (k_msgq_get(&nav_q, &it, K_NO_WAIT) == 0) {
		switch (it.ev) {
		case QE_FOUND:
			on_found(&it);
			break;
		case QE_CONNECTED:
			on_connected(&it);
			break;
		case QE_INCOMING:
			on_incoming(&it);
			break;
		case QE_SECURED:
			on_secured(&it);
			break;
		case QE_DISCONNECTED:
			on_disconnected(&it);
			break;
		case QE_HIS:
			on_his(&it);
			break;
		case QE_HELLO:
			on_hello(&it);
			break;
		case QE_CS_FAIL:
			if (lk[it.a].st == L_UP && lk[it.a].central) {
				link_drop(it.a, CS_RETRY_MS);
			}
			break;
		case QE_TICK:
			on_tick();
			break;
		case QE_CMD:
			if (it.a == 1 || (it.a == 2 && !atomic_get(&active))) {
				mode_on();
			} else {
				mode_off();
			}
			break;
		default:
			break;
		}
		if (it.conn != NULL) {
			bt_conn_unref(it.conn);
		}
	}
	if (atomic_get(&active)) {
		scan_update();
	}
}

void nav_toggle(void)
{
	q_post(QE_CMD, 2, 0, NULL, NULL, 0);
}

bool nav_active(void)
{
	return atomic_get(&active) != 0;
}

void nav_shutdown(void)
{
	atomic_set(&active, 0);
	k_timer_stop(&tick_timer);
	(void)k_work_cancel_delayable(&tx_work);
	for (int i = 0; i < LK_COUNT; i++) {
		eng_stop(i);
		if (lk[i].conn != NULL) {
			(void)bt_conn_disconnect(lk[i].conn, BT_HCI_ERR_REMOTE_POWER_OFF);
		}
	}
}

/* ---------------- Callback Bluetooth (bt_rx) tu main.c / ic_service.c ---------------- */

bool nav_conn_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	bt_addr_le_t dst;
	k_spinlock_key_t key;
	bool match;
	uint8_t i;

	if (bt_conn_get_info(conn, &info) != 0) {
		return false;
	}
	if (info.role == BT_CONN_ROLE_PERIPHERAL) {
		/* Dang Dinh vi: moi ket noi den la ung vien mu ban (khong gui HELLO -> dien thoai) */
		if (err || !atomic_get(&active)) {
			return false;
		}
		own_add(conn);
		q_post(QE_INCOMING, 0, 0, conn, NULL, 0);
		return true;
	}
	if (!atomic_get(&connecting)) {
		return false;
	}
	nav_addr_norm(&dst, bt_conn_get_dst(conn));
	key = k_spin_lock(&lock);
	match = bt_addr_le_eq(&dst, &target_addr);
	i = target_lk;
	k_spin_unlock(&lock, key);
	if (!match) {
		return false;
	}
	atomic_clear(&connecting);
	if (err == 0) {
		own_add(conn);
	}
	q_post(QE_CONNECTED, i, err, conn, NULL, 0);
	return true;
}

bool nav_conn_owned(const struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const bool r = owned_has(conn);

	k_spin_unlock(&lock, key);
	return r;
}

void nav_conn_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(level);
	q_post(QE_SECURED, err ? 1 : 0, (uint8_t)err, conn, NULL, 0);
}

void nav_conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
	q_post(QE_DISCONNECTED, reason, 0, conn, NULL, 0);
	own_remove(conn);
}

void nav_on_his_discovered(struct bt_conn *conn, int err)
{
	q_post(QE_HIS, err ? 1 : 0, 0, conn, NULL, 0);
}

void nav_on_his_msg(struct bt_conn *conn, uint8_t op, uint8_t age100, uint16_t val)
{
	const int64_t ts = k_uptime_get() - (int64_t)age100 * 100;
	k_spinlock_key_t key;

	switch (op) {
	case IC_OP_NAV_HELLO:
		q_post(QE_HELLO, (uint8_t)val, 0, conn, NULL, 0);
		break;
	case IC_OP_NAV_DIST_PEER:
		key = k_spin_lock(&lock);
		if (tx_conn == conn) {
			rx_peer_cm = val;
			rx_peer_ts = ts;
		}
		k_spin_unlock(&lock, key);
		break;
	case IC_OP_NAV_DIST_BIKE:
		key = k_spin_lock(&lock);
		if (tx_conn == conn) {
			rx_pb_cm = val;
			rx_pb_ts = ts;
		}
		k_spin_unlock(&lock, key);
		break;
	default:
		break;
	}
}

/* ---------------- Shell / khoi tao ---------------- */

static void print_link(const struct shell *sh, int i)
{
	const struct link *l = &lk[i];
	const struct cs_eng *e = &eng[i];
	char addr[BT_ADDR_LE_STR_LEN] = "-";
	char d[20];

	if (l->conn != NULL) {
		conn_str(l->conn, addr, sizeof(addr));
	}
	shell_print(sh, "%-10s: %s, %s%s", i == LK_BIKE ? "Bike" : "Peer", addr, lst_names[l->st],
		    l->st != L_UP ? "" : (l->central ? ", this helmet measures"
						     : ", the other helmet measures"));
	if (l->central && l->st == L_UP) {
		int64_t age = 0;

		cm_str(own_cm(i, k_uptime_get(), HOLD_MS, &age), d, sizeof(d));
		shell_print(sh, "  CS      : %s, distance %s (quality %u, median of %u), %u channels",
			    cs_names[atomic_get(&e->st)], d, e->q, e->est_n, e->n_channels);
		shell_print(sh, "  procs   : %u ok, %u aborted (last proc %u / subevent %u), "
			    "%u dropped, %u invalid",
			    e->n_ok, e->n_abort, e->last_proc_abort, e->last_sub_abort, e->n_drop,
			    e->n_bad);
	}
}

int nav_shell(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "on") == 0) {
		q_post(QE_CMD, 1, 0, NULL, NULL, 0);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "off") == 0) {
		q_post(QE_CMD, 0, 0, NULL, NULL, 0);
		return 0;
	}
	if (argc == 3) {
		const long v = strtol(argv[2], NULL, 10);

		if (strcmp(argv[1], "ch") == 0 && (v == 1 || v == 2 || v == 4)) {
			bike_stride = (uint8_t)v;
		} else if (strcmp(argv[1], "period") == 0 && v >= PERIOD_MIN_MS &&
			   v <= PERIOD_MAX_MS) {
			bike_period_ms = (uint16_t)v;
		} else {
			shell_error(sh, "usage: helmet nav ch <1|2|4> | period <%d..%d ms> (bike link)",
				    PERIOD_MIN_MS, PERIOD_MAX_MS);
			return -EINVAL;
		}
		shell_print(sh, "Bike link: applied from the next connection to the bike");
		return 0;
	}
	if (argc != 1 && !(argc == 2 && strcmp(argv[1], "status") == 0)) {
		shell_error(sh, "usage: helmet nav [status|on|off|ch <1|2|4>|period <ms>]");
		return -EINVAL;
	}
	shell_print(sh, "Nav       : %s, tag %08x; bike link: every %u channel(s), procedure %u ms, "
		    "subevent <= %u us",
		    atomic_get(&active) ? "ON" : "off (hold Call 1 s)", my_tag, bike_stride,
		    bike_period_ms, BIKE_SUB_MAX_US);
	if (atomic_get(&active)) {
		print_link(sh, LK_BIKE);
		print_link(sh, LK_PEER);
		log_line();
		log_stats();
	}
	return 0;
}

int nav_init(void)
{
	for (int i = 0; i < LK_COUNT; i++) {
		k_work_init(&eng[i].dist_work, dist_fn);
	}
	bt_le_scan_cb_register(&scan_cb);
	return 0;
}
