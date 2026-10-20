/*
 * Helmet - do khoang cach mu-mu bang Channel Sounding (Bluetooth 6.0) trong luc goi intercom.
 * Thiet ke: docs/HELMET_CHANNEL_SOUNDING.md
 *
 * Dung CS Inline PCT Transfer (IPT, "CS enhancements"): ben phan xa chinh pha tone gui lai theo
 * pha tone vua nhan, nen ben khoi tao tu tinh khoang cach chi tu du lieu cua chinh no - khong
 * can Ranging Service (GATT), it RAM, it radio. Chi dung buoc mode 2 (PBR), 1 duong anten.
 *
 *   Master intercom (central) = ben KHOI TAO: khi am thanh intercom da chay va ACL da doi sang
 *     chu ky 60 ms -> cai dat mac dinh (initiator) -> doc kha nang CS ben kia -> tao cau hinh
 *     (mode 2, IPT) -> bat bao mat CS -> tham so thu tuc (~1 thu tuc / s) -> bat thu tuc.
 *     Moi thu tuc xong: PCT tung kenh -> cs_de_ifft() -> trung vi 9 mau -> log + gui sang slave
 *     (HIS IC_OP_RANGE, cm).
 *   Slave intercom (peripheral) = ben PHAN XA: khi LINKED -> cai dat mac dinh (reflector); log
 *     khoang cach master gui sang.
 *
 * Subevent CS toi da 5 ms: ngan hon khoang trong giua hai su kien CIS (chu ky 10 ms) cua am
 * thanh intercom. Tranh radio voi CIS thi bot kenh: "helmet cs ch 2" (hoac 4).
 *
 * Luong: callback Bluetooth (bt_rx) chi ghi trang thai / du lieu va dua viec vao system
 * workqueue; lenh HCI (dong bo, vai ms) va IFFT chay trong work. Ngat ket noi -> tu dung.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
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
#include <zephyr/sys/byteorder.h>
#include <bluetooth/cs_de.h>

#include "helmet.h"

LOG_MODULE_REGISTER(ranging, LOG_LEVEL_INF);

#define CS_CONFIG_ID     0
#define CS_MODE0_STEPS   3
#define CH_INDEX_OFFSET  2    /* kenh CS 2..76 -> iq.values[0..74] */
#define CH_COUNT_MAX     79   /* bit 0..78 cua channel map */
#define MEDIAN_N         9
#define LINK_SETTLE_MS   3000 /* cho ACL doi chu ky; qua han thi chay voi chu ky hien tai */
#define SUBEVENT_MIN_US  2500
#define SUBEVENT_MAX_US  5000 /* < khoang trong giua 2 su kien CIS 10 ms */
#define PROC_MAX_EVENTS  6    /* mot thu tuc trai toi da 6 chu ky ACL */
#define PERIOD_MIN_MS    200
#define PERIOD_MAX_MS    5000

enum rs {
	RS_OFF,
	RS_REFL,        /* slave: dat cai dat mac dinh (reflector) */
	RS_REFL_READY,  /* slave: cho master do */
	RS_WAIT_LINK,   /* master: cho ACL doi sang chu ky luc goi */
	RS_CAPS,        /* master: cai dat mac dinh + doc kha nang ben kia */
	RS_WAIT_CAPS,
	RS_CONFIG,      /* tao cau hinh (mode 2, IPT) */
	RS_WAIT_CONFIG,
	RS_SEC,         /* bao mat CS */
	RS_WAIT_SEC,
	RS_ENABLE,      /* tham so thu tuc + bat */
	RS_WAIT_ENABLE,
	RS_RUNNING,
	RS_FAILED,
};

static const char *const rs_names[] = {
	"off", "reflector setup", "reflector ready", "waiting link", "capabilities",
	"capabilities", "config", "config", "security", "security", "enable", "enable",
	"RUNNING", "FAILED",
};
BUILD_ASSERT(ARRAY_SIZE(rs_names) == RS_FAILED + 1);

static struct k_spinlock lock;
static struct bt_conn *rconn; /* lien ket intercom dang do (giu 1 ref) */
static atomic_t state = ATOMIC_INIT(RS_OFF);
static bool initiator;

/* Chinh bang shell, ap dung tu lan goi sau */
static uint8_t ch_stride = 1;     /* 1: moi kenh; 2 / 4: bo bot kenh, it chiem radio */
static uint16_t period_ms = 1000; /* chu ky thu tuc mong muon */

/* Thong ke (bt_rx ghi, work / shell doc: chi de hien thi) */
static uint32_t n_ok, n_abort, n_drop, n_bad;
static uint8_t last_proc_abort, last_sub_abort;
static uint8_t n_channels;
static struct bt_conn_le_cs_procedure_enable_complete proc_info;
static uint16_t last_cm = UINT16_MAX, median_cm = UINT16_MAX;
static uint16_t peer_cm = UINT16_MAX; /* slave: master gui sang */
static int64_t peer_ts;

/* IQ cua ben khoi tao theo kenh; cs_de_ifft dung chung vung nay lam bo nho tam IFFT */
static union {
	struct {
		float i;
		float q;
	} values[CS_DE_NUM_CHANNELS];
	float scratch[2 * CONFIG_BT_CS_DE_NFFT_SIZE];
} iq;

/* Thu tuc dang gom / bo qua (bt_rx); dat lai moi cuoc goi */
static uint32_t cur_proc, dropped_proc, aborted_proc;

static float est[MEDIAN_N];
static uint8_t est_n, est_idx;
static int64_t est_ts; /* thoi diem uoc luong truoc */

static void step_fn(struct k_work *w);
static void dist_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(step_work, step_fn);
static K_WORK_DEFINE(dist_work, dist_fn);

/* ---------------- Tien ich ---------------- */

static struct bt_conn *conn_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bt_conn *c = rconn != NULL ? bt_conn_ref(rconn) : NULL;

	k_spin_unlock(&lock, key);
	return c;
}

static bool is_ours(const struct bt_conn *conn)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const bool r = rconn != NULL && rconn == conn;

	k_spin_unlock(&lock, key);
	return r;
}

static uint16_t m_to_cm(float m)
{
	if (!isfinite(m) || m < 0.0f) {
		return UINT16_MAX;
	}
	return (uint16_t)MIN(lroundf(m * 100.0f), UINT16_MAX - 1);
}

static void cm_str(uint16_t cm, char *buf, size_t len)
{
	if (cm == UINT16_MAX) {
		snprintk(buf, len, "-");
	} else {
		snprintk(buf, len, "%u.%02u m", cm / 100U, cm % 100U);
	}
}

static void fail(const char *what, int err)
{
	atomic_set(&state, RS_FAILED);
	LOG_WRN("Channel Sounding: %s failed (%d) - no ranging for this call", what, err);
}

/* Callback (bt_rx): buoc cho xong -> buoc lam tiep theo (trong work) */
static void advance(enum rs from, enum rs to)
{
	if (atomic_cas(&state, from, to)) {
		k_work_reschedule(&step_work, K_NO_WAIT);
	}
}

static int set_default_settings(struct bt_conn *c, bool init_role)
{
	const struct bt_le_cs_set_default_settings_param p = {
		.enable_initiator_role = init_role,
		.enable_reflector_role = !init_role,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};

	return bt_le_cs_set_default_settings(c, &p);
}

static void config_get(struct bt_le_cs_create_config_params *p)
{
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
	n_channels = 0;
	for (uint8_t ch = 0; ch < CH_COUNT_MAX; ch++) {
		if (ch_stride > 1 && (ch % ch_stride) != 0) {
			p->channel_map[ch / 8] &= ~BIT(ch % 8);
		}
		if (p->channel_map[ch / 8] & BIT(ch % 8)) {
			n_channels++;
		}
	}
}

static int set_procedure_params(struct bt_conn *c)
{
	struct bt_conn_info info;
	uint32_t int_us = 60000U;
	uint16_t acl_units;
	uint16_t interval_ev;

	if (bt_conn_get_info(c, &info) == 0 && info.le.interval_us > 0) {
		int_us = info.le.interval_us;
	}
	acl_units = (uint16_t)(int_us / 625U); /* don vi 0,625 ms */
	interval_ev = (uint16_t)MAX(2U, DIV_ROUND_CLOSEST(period_ms * 1000U, int_us));

	const struct bt_le_cs_set_procedure_parameters_param p = {
		.config_id = CS_CONFIG_ID,
		.max_procedure_len = acl_units * MIN(PROC_MAX_EVENTS, interval_ev - 1),
		.min_procedure_interval = interval_ev,
		.max_procedure_interval = interval_ev,
		.max_procedure_count = 0, /* lap toi khi tat / ngat */
		.min_subevent_len = SUBEVENT_MIN_US,
		.max_subevent_len = SUBEVENT_MAX_US,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80, /* khong yeu cau */
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	LOG_INF("Channel Sounding: ACL %u.%02u ms -> 1 procedure every %u events (~%u ms), "
		"subevent %u-%u us, %u channels",
		int_us / 1000U, (int_us % 1000U) / 10U, interval_ev, interval_ev * int_us / 1000U,
		SUBEVENT_MIN_US, SUBEVENT_MAX_US, n_channels);
	return bt_le_cs_set_procedure_parameters(c, &p);
}

/* ---------------- Cac buoc (system workqueue) ---------------- */

static void step_fn(struct k_work *w)
{
	struct bt_conn *c = conn_get();
	struct bt_le_cs_create_config_params cfg;
	int err;

	ARG_UNUSED(w);
	if (c == NULL) {
		return;
	}

	switch ((enum rs)atomic_get(&state)) {
	case RS_REFL:
		err = set_default_settings(c, false);
		if (err) {
			fail("reflector setup", err);
			break;
		}
		atomic_cas(&state, RS_REFL, RS_REFL_READY);
		LOG_INF("Channel Sounding: reflector ready - the master helmet measures");
		break;

	case RS_WAIT_LINK:
		/* Het han cho ACL doi chu ky: chay voi chu ky hien tai */
		advance(RS_WAIT_LINK, RS_CAPS);
		break;

	case RS_CAPS:
		err = set_default_settings(c, true);
		if (err == 0) {
			atomic_set(&state, RS_WAIT_CAPS);
			err = bt_le_cs_read_remote_supported_capabilities(c);
		}
		if (err) {
			fail("capability exchange", err);
		}
		break;

	case RS_CONFIG:
		config_get(&cfg);
		atomic_set(&state, RS_WAIT_CONFIG);
		err = bt_le_cs_create_config(c, &cfg, BT_LE_CS_CREATE_CONFIG_CONTEXT_LOCAL_AND_REMOTE);
		if (err) {
			fail("config create", err);
		}
		break;

	case RS_SEC:
		atomic_set(&state, RS_WAIT_SEC);
		err = bt_le_cs_security_enable(c);
		if (err) {
			fail("CS security", err);
		}
		break;

	case RS_ENABLE: {
		const struct bt_le_cs_procedure_enable_param ep = {
			.config_id = CS_CONFIG_ID,
			.enable = BT_CONN_LE_CS_PROCEDURES_ENABLED,
		};

		atomic_set(&state, RS_WAIT_ENABLE);
		err = set_procedure_params(c);
		if (err == 0) {
			err = bt_le_cs_procedure_enable(c, &ep);
		}
		if (err) {
			fail("procedure enable", err);
		}
		break;
	}

	default:
		break;
	}
	bt_conn_unref(c);
}

/* ---------------- Uoc luong khoang cach (system workqueue) ---------------- */

static int float_cmp(const void *a, const void *b)
{
	const float fa = *(const float *)a;
	const float fb = *(const float *)b;

	return (fa > fb) - (fa < fb);
}

static float median_of_est(void)
{
	float tmp[MEDIAN_N];
	uint8_t n = 0;

	for (uint8_t i = 0; i < est_n; i++) {
		if (isfinite(est[i])) {
			tmp[n++] = est[i];
		}
	}
	if (n == 0) {
		return NAN;
	}
	qsort(tmp, n, sizeof(float), float_cmp);
	return (n % 2) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0f;
}

static void dist_fn(struct k_work *w)
{
	const float d = cs_de_ifft(iq.scratch);
	const int64_t now = k_uptime_get();
	char s_now[12];
	char s_med[12];
	struct bt_conn *c;

	ARG_UNUSED(w);
	if (!isfinite(d)) {
		n_bad++;
		return;
	}
	est[est_idx] = d;
	est_idx = (est_idx + 1) % MEDIAN_N;
	if (est_n < MEDIAN_N) {
		est_n++;
	}
	last_cm = m_to_cm(d);
	median_cm = m_to_cm(median_of_est());

	cm_str(median_cm, s_med, sizeof(s_med));
	cm_str(last_cm, s_now, sizeof(s_now));
	LOG_INF("Distance to peer helmet: %s (now %s, median of %u, +%lld ms)", s_med, s_now,
		est_n, est_ts ? now - est_ts : 0);
	est_ts = now;

	/* Gui sang slave; dang ban (vd END dang gui) thi bo lan nay */
	c = conn_get();
	if (c != NULL) {
		if (atomic_get(&state) == RS_RUNNING && !ic_svc_busy()) {
			(void)ic_svc_send2(c, true, IC_OP_RANGE, median_cm & 0xFF, median_cm >> 8);
		}
		bt_conn_unref(c);
	}
}

/* ---------------- Callback Bluetooth (bt_rx) ---------------- */

static void param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			  uint16_t timeout)
{
	ARG_UNUSED(interval);
	ARG_UNUSED(latency);
	ARG_UNUSED(timeout);
	if (is_ours(conn)) {
		advance(RS_WAIT_LINK, RS_CAPS);
	}
}

static void remote_caps_cb(struct bt_conn *conn, uint8_t status,
			   struct bt_conn_le_cs_capabilities *p)
{
	if (!is_ours(conn) || atomic_get(&state) != RS_WAIT_CAPS) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		fail("capability exchange", -status);
		return;
	}
	LOG_INF("Channel Sounding: peer reflector %s, IPT %s (t_ip2 0x%04x, t_sw %u us)",
		p->reflector_supported ? "yes" : "NO",
		p->t_ip2_ipt_times_supported ? "yes" : "NO", p->t_ip2_ipt_times_supported,
		p->t_sw_ipt_time_supported);
	if (!p->reflector_supported || p->t_ip2_ipt_times_supported == 0) {
		fail("peer without CS reflector / IPT", -ENOTSUP);
		return;
	}
	advance(RS_WAIT_CAPS, RS_CONFIG);
}

static void config_cb(struct bt_conn *conn, uint8_t status, struct bt_conn_le_cs_config *cfg)
{
	if (!is_ours(conn)) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		if (initiator) {
			fail("config create", -status);
		} else {
			LOG_WRN("Channel Sounding: config from master failed (0x%02x)", status);
		}
		return;
	}
	LOG_INF("Channel Sounding: config %u created (%s, mode %u, T_IP2 %u us, T_FCS %u us, "
		"T_PM %u us)",
		cfg->id, initiator ? "initiator" : "reflector", cfg->mode, cfg->t_ip2_time_us,
		cfg->t_fcs_time_us, cfg->t_pm_time_us);
	advance(RS_WAIT_CONFIG, RS_SEC);
}

static void security_cb(struct bt_conn *conn, uint8_t status)
{
	if (!is_ours(conn)) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		if (initiator) {
			fail("CS security", -status);
		} else {
			LOG_WRN("Channel Sounding: CS security failed (0x%02x)", status);
		}
		return;
	}
	advance(RS_WAIT_SEC, RS_ENABLE);
}

static void proc_enable_cb(struct bt_conn *conn, uint8_t status,
			   struct bt_conn_le_cs_procedure_enable_complete *p)
{
	if (!is_ours(conn)) {
		return;
	}
	if (status != BT_HCI_ERR_SUCCESS) {
		if (initiator) {
			fail("procedure enable", -status);
		} else {
			LOG_WRN("Channel Sounding: procedure enable failed (0x%02x)", status);
		}
		return;
	}
	proc_info = *p;
	if (p->state != BT_CONN_LE_CS_PROCEDURES_ENABLED) {
		LOG_INF("Channel Sounding: procedures disabled");
		return;
	}
	LOG_INF("Channel Sounding: RUNNING as %s - subevent %u us x %u/event, event every %u "
		"conn events, procedure every %u conn events, max length %u x 0.625 ms, TX %d dBm",
		initiator ? "initiator" : "reflector", p->subevent_len, p->subevents_per_event,
		p->event_interval, p->procedure_interval, p->max_procedure_len,
		p->selected_tx_power);
	if (initiator) {
		advance(RS_WAIT_ENABLE, RS_RUNNING);
	}
}

/* Lay PCT cua tung buoc mode 2 vao iq.values theo kenh */
static void steps_parse(struct bt_conn_le_cs_subevent_result *r)
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

			iq.values[step.channel - CH_INDEX_OFFSET].i = s.i;
			iq.values[step.channel - CH_INDEX_OFFSET].q = s.q;
		}
		net_buf_simple_pull(r->step_data_buf, step.data_len);
	}
}

static void subevent_cb(struct bt_conn *conn, struct bt_conn_le_cs_subevent_result *r)
{
	const uint16_t pc = r->header.procedure_counter;

	if (!is_ours(conn)) {
		return;
	}
	if (r->header.procedure_abort_reason != BT_HCI_LE_CS_PROCEDURE_ABORT_REASON_NO_ABORT ||
	    r->header.subevent_abort_reason != BT_HCI_LE_CS_SUBEVENT_ABORT_REASON_NO_ABORT) {
		if (aborted_proc != pc) {
			aborted_proc = pc;
			n_abort++;
			last_proc_abort = r->header.procedure_abort_reason;
			last_sub_abort = r->header.subevent_abort_reason;
		}
		return;
	}
	if (!initiator) {
		/* Ben phan xa: chi dem de biet CS dang chay */
		if (r->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
			n_ok++;
		}
		return;
	}
	if (atomic_get(&state) != RS_RUNNING || pc == dropped_proc) {
		return;
	}
	if (k_work_is_pending(&dist_work)) {
		/* Dang tinh thu tuc truoc (dung chung bo dem): bo thu tuc nay */
		dropped_proc = pc;
		n_drop++;
		return;
	}
	if (pc != cur_proc) {
		memset(iq.scratch, 0, sizeof(iq.scratch));
		cur_proc = pc;
	}
	steps_parse(r);
	if (r->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
		n_ok++;
		k_work_submit(&dist_work);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bt_conn *drop = NULL;

	ARG_UNUSED(reason);
	if (rconn != NULL && rconn == conn) {
		drop = rconn;
		rconn = NULL;
	}
	k_spin_unlock(&lock, key);
	if (drop == NULL) {
		return;
	}
	atomic_set(&state, RS_OFF);
	(void)k_work_cancel_delayable(&step_work);
	LOG_INF("Channel Sounding: stopped (%u procedures, %u aborted)", n_ok, n_abort);
	bt_conn_unref(drop);
}

BT_CONN_CB_DEFINE(ranging_conn_cb) = {
	.disconnected = disconnected,
	.le_param_updated = param_updated,
	.le_cs_read_remote_capabilities_complete = remote_caps_cb,
	.le_cs_config_complete = config_cb,
	.le_cs_security_enable_complete = security_cb,
	.le_cs_procedure_enable_complete = proc_enable_cb,
	.le_cs_subevent_data_available = subevent_cb,
};

/* ---------------- API ---------------- */

void ranging_start(struct bt_conn *conn, bool as_initiator)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bt_conn *old = NULL;

	if (rconn == conn) {
		k_spin_unlock(&lock, key);
		return; /* da chay tren lien ket nay */
	}
	old = rconn;
	rconn = bt_conn_ref(conn);
	k_spin_unlock(&lock, key);
	if (old != NULL) {
		bt_conn_unref(old);
	}

	initiator = as_initiator;
	n_ok = n_abort = n_drop = n_bad = 0;
	est_n = est_idx = 0;
	est_ts = 0;
	cur_proc = dropped_proc = aborted_proc = UINT32_MAX; /* bo dem thu tuc 16 bit: khong trung */
	last_cm = median_cm = peer_cm = UINT16_MAX;
	memset(&proc_info, 0, sizeof(proc_info));

	if (as_initiator) {
		LOG_INF("Channel Sounding: initiator - starting after the call link settles");
		atomic_set(&state, RS_WAIT_LINK);
		k_work_reschedule(&step_work, K_MSEC(LINK_SETTLE_MS));
	} else {
		atomic_set(&state, RS_REFL);
		k_work_reschedule(&step_work, K_NO_WAIT);
	}
}

void ranging_on_peer_report(uint16_t cm)
{
	char s[12];

	if (atomic_get(&state) == RS_OFF || initiator) {
		return;
	}
	peer_cm = cm;
	peer_ts = k_uptime_get();
	cm_str(cm, s, sizeof(s));
	LOG_INF("Distance to peer helmet: %s (measured by master)", s);
}

void ranging_log_status(void)
{
	const enum rs s = (enum rs)atomic_get(&state);
	char d[12];

	if (s == RS_OFF) {
		return;
	}
	if (initiator) {
		cm_str(median_cm, d, sizeof(d));
		LOG_INF("ranging: %s, distance %s, procedures %u ok / %u aborted / %u dropped, "
			"%u invalid",
			rs_names[s], d, n_ok, n_abort, n_drop, n_bad);
	} else {
		cm_str(peer_cm, d, sizeof(d));
		LOG_INF("ranging: reflector %s, procedures %u ok / %u aborted, distance %s",
			rs_names[s], n_ok, n_abort, d);
	}
}

int ranging_shell(const struct shell *sh, size_t argc, char **argv)
{
	const enum rs s = (enum rs)atomic_get(&state);
	char d[12];

	if (argc == 3 && strcmp(argv[1], "ch") == 0) {
		const long v = strtol(argv[2], NULL, 10);

		if (v != 1 && v != 2 && v != 4) {
			shell_error(sh, "usage: helmet cs ch <1|2|4>");
			return -EINVAL;
		}
		ch_stride = (uint8_t)v;
		shell_print(sh, "Channel stride %ld (from the next call)", v);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "period") == 0) {
		const long v = strtol(argv[2], NULL, 10);

		if (v < PERIOD_MIN_MS || v > PERIOD_MAX_MS) {
			shell_error(sh, "usage: helmet cs period <%d..%d ms>", PERIOD_MIN_MS,
				    PERIOD_MAX_MS);
			return -EINVAL;
		}
		period_ms = (uint16_t)v;
		shell_print(sh, "Procedure period %ld ms (from the next call)", v);
		return 0;
	}
	if (argc != 1) {
		shell_error(sh, "usage: helmet cs [ch <1|2|4> | period <ms>]");
		return -EINVAL;
	}

	shell_print(sh, "Ranging   : %s, role %s, channel stride %u, period %u ms", rs_names[s],
		    s == RS_OFF ? "-" : (initiator ? "initiator" : "reflector"), ch_stride,
		    period_ms);
	if (initiator) {
		shell_print(sh, "Channels  : %u", n_channels);
		cm_str(median_cm, d, sizeof(d));
		shell_print(sh, "Distance  : %s (median of %u)", d, est_n);
		cm_str(last_cm, d, sizeof(d));
		shell_print(sh, "Last      : %s", d);
	} else {
		cm_str(peer_cm, d, sizeof(d));
		shell_print(sh, "Distance  : %s (from master, %lld ms ago)", d,
			    peer_cm == UINT16_MAX ? 0 : k_uptime_get() - peer_ts);
	}
	shell_print(sh, "Procedures: %u ok, %u aborted (last reason proc %u / subevent %u), "
		    "%u dropped, %u invalid",
		    n_ok, n_abort, last_proc_abort, last_sub_abort, n_drop, n_bad);
	if (proc_info.state == BT_CONN_LE_CS_PROCEDURES_ENABLED) {
		shell_print(sh, "Schedule  : subevent %u us x %u/event, event every %u, procedure "
			    "every %u conn events",
			    proc_info.subevent_len, proc_info.subevents_per_event,
			    proc_info.event_interval, proc_info.procedure_interval);
	}
	return 0;
}
