/*
 * Helmet - Auracast: nghe chung nhac tu mot dien thoai
 *          (BAP Broadcast Sink + Scan Delegator / Broadcast Audio Scan Service)
 * Thiet ke va cach dung: docs/HELMET_AURACAST.md
 *
 * Hai cach vao mot broadcast:
 *  1) Dien thoai la Broadcast Assistant (Android "Chia se am thanh" / Auracast): dien thoai ghi
 *     BASS Add Source -> pa_sync_req -> mu dong bo PA (PAST qua ket noi ACL, hoac tu quet theo
 *     dia chi + SID dien thoai gui) -> BASE + BIGInfo -> (ma broadcast tu dien thoai, neu co)
 *     -> yeu cau BIS -> dong bo BIG -> nhan BIS -> loa.
 *  2) Tu tim (khong co assistant): Power ngan -> giu Call 2 s, hoac "helmet bc scan [ten]":
 *     quet Broadcast Audio Announcement, chon cai manh nhat, tu dong bo PA -> nhu tren.
 *
 * Luong BIS dung chung duong giai ma / loa voi unicast (audio_out, khe 0/1). Unicast (cuoc goi,
 * nhac qua CIS, intercom slave) uu tien: khi co luong sink unicast, BIS van dong bo nhung khong
 * dua ra loa (bcast_audio_hold). Intercom va Auracast loai tru nhau.
 *
 * Nguon tam dung (vd may tinh bang dung nhac): BIG ket thuc nhung PA van con -> mu cho BIGInfo
 * moi roi tu dong bo lai, khong roi broadcast. Da co PA + BASE thi cho BIGInfo / ma khong gioi
 * han (toi khi nguoi dung roi bang nut hoac mat PA). Ma nhap bang "helmet bc code" duoc luu
 * (settings "bcast/code") va dung ngay ca khi dang cho.
 *
 * Bike hub (apps/bike): trong luc tu tim, thay hub dang mo cua so ghep (Service Data BHS) ->
 * hub_link.c ket noi hub, doc broadcast ID + dia chi + SID + ma -> vao ngay va LUU ("bcast/hub").
 * Moi lan bat (va khi broadcast cua hub mat) mu tu dong bo lai theo dia chi + SID da luu, khong
 * can bam nut, khong go ma. Roi broadcast bang nut thi thoi tu vao lai toi lan ghep / bat sau.
 *
 * Luong thuc thi: callback Bluetooth (bt_rx) chi chep du lieu + bat co su kien roi
 * app_post(EVT_BC); moi quyet dinh co lenh HCI chay trong bcast_handle() o thread bo dieu khien
 * (app/app_ctrl.c), cung thread voi intercom. Rieng duong am thanh (started/stopped/recv cua luong BIS)
 * chay thang trong callback, nhu le_audio.c.
 */

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <me54/services/audio_stream.h>
#include <me54/services/audio_out.h>

#include "app/app.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/phone/le_audio.h"

LOG_MODULE_REGISTER(bcast, LOG_LEVEL_INF);

#define BC_STREAMS CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT
#define BC_MAX_SG  CONFIG_BT_BAP_BASS_MAX_SUBGROUPS

#define BC_MAX_BIS_SG      4    /* so BIS ghi nho moi subgroup cua BASE */
#define BC_NAME_LEN        33   /* Broadcast Name toi da 32 byte UTF-8 */
#define BC_SCAN_TIMEOUT_S  20   /* tu tim: het 20 s khong thay broadcast nao -> bao loi */
#define BC_CHOOSE_MS       2000 /* sau broadcast dau tien: nghe them 2 s, chon cai manh nhat */
#define BC_PA_TIMEOUT_S    10   /* cho PAST / dong bo PA */
#define BC_SETUP_TIMEOUT_S 15   /* tu tim: cho BASE + BIGInfo (+ ma) sau khi co PA */
#define BC_MAX_RETRY       3
/* Sau khi dong bo, controller duoc bo qua toi 2 su kien PA lien tiep (BASE / BIGInfo lap lai
 * moi su kien, khong can nghe het) -> bot thoi gian radio danh cho CIS voi dien thoai.
 */
#define BC_PA_SKIP 2

/* ---------------- BASE da phan tich ---------------- */

struct bc_bis_info {
	uint8_t index;
	bool ok; /* LC3 ma audio_out phat duoc */
	struct audio_stream_cfg cfg;
};

struct bc_sg_info {
	uint8_t n_bis;
	struct bc_bis_info bis[BC_MAX_BIS_SG];
};

struct bc_base_info {
	int32_t pd_us;
	uint8_t n_sg;
	struct bc_sg_info sg[BC_MAX_SG];
};

/* Mot nguon broadcast: dia chi + SID cua bo quang ba mo rong, Broadcast ID (BAP 6.5.4) */
struct bc_target {
	bt_addr_le_t addr;
	uint8_t sid;
	uint32_t broadcast_id;
	uint16_t pa_interval; /* don vi 1.25 ms, BT_BAP_PA_INTERVAL_UNKNOWN neu khong biet */
	uint8_t src_id;       /* Source ID trong Receive State cua BASS */
	bool src_valid;
	char name[BC_NAME_LEN];
};

/* ---------------- Su kien (callback -> bcast_handle) ---------------- */

enum {
	EV_PA_REQ,      /* dien thoai: Add/Modify Source xin dong bo PA */
	EV_PA_TERM,     /* dien thoai: xin ngung PA */
	EV_SRC_REMOVED, /* dien thoai: Remove Source */
	EV_PA_SYNCED,
	EV_PA_LOST,
	EV_BASE,
	EV_SYNCABLE, /* da nhan BIGInfo */
	EV_CODE,     /* ma broadcast tu dien thoai */
	EV_BIS_REQ,  /* dien thoai: BIS can dong bo moi subgroup */
	EV_BIG_STARTED,
	EV_BIG_STOPPED,
	EV_T_PHASE,
	EV_T_CHOOSE,
	EV_T_RETRY,
	EV_CMD_SCAN,
	EV_CMD_STOP,
	EV_USER_CODE, /* "helmet bc code": ma moi, dung ngay neu dang cho ma */
	EV_HUB_FOUND,  /* luc tu tim: thay Bike hub dang mo cua so ghep */
	EV_HUB_RESULT, /* hub_link.c: doc Join Info xong (hoac loi) */
	EV_REJOIN,     /* hen gio: thu tu vao lai broadcast cua hub da ghep */
};

static atomic_t ev_bits;

static void post(int ev)
{
	atomic_set_bit(&ev_bits, ev);
	app_post(EVT_BC, 0, 0, 0);
}

/* ---------------- Du lieu chung callback <-> bcast_handle (giu lock) ---------------- */

static struct k_spinlock lock;

static struct bc_target req_tgt;
static bool req_past;
static struct bt_conn *req_conn; /* giu 1 ref, bcast_handle nhan lai */
static uint8_t term_src_id;
static uint8_t removed_src_id;
static uint32_t req_bis[BC_MAX_SG];
static uint8_t req_nsg;
static uint8_t req_code[BT_ISO_BROADCAST_CODE_SIZE];

static bool want_create; /* dang tao dong bo PA voi SID want_sid */
static bool want_past;   /* dang cho PAST voi SID want_sid */
static uint8_t want_sid;
static struct bt_le_per_adv_sync *synced_pa; /* PA vua dong bo, cho bcast_handle nhan */
static struct bt_le_per_adv_sync *cur_pa;    /* PA dang dung: nhan ra lan mat dong bo cua minh */
static uint8_t pa_lost_reason;
static uint8_t big_stop_reason;

static uint8_t base_raw[255]; /* BASE gan nhat (chi phan tich lai khi doi) */
static uint16_t base_raw_len;
static struct bc_base_info base_new;
static bool enc_new;

/* Thong so BIG (tu BIGInfo), chi de ghi log mot lan moi phien */
struct bc_big_params {
	uint8_t num_bis;
	uint8_t nse;
	uint16_t iso_interval; /* don vi 1.25 ms */
	uint8_t bn;
	uint8_t pto;
	uint8_t irc;
	uint16_t max_pdu;
	uint32_t sdu_interval;
	uint16_t max_sdu;
	uint8_t phy;
	uint8_t framing;
};
static struct bc_big_params big_new;
static atomic_t syncable_a; /* 0 chua co BIGInfo, 1 khong ma hoa, 2 ma hoa */

static struct bc_target cand; /* tu tim: broadcast manh nhat */
static int8_t cand_rssi;
static bool cand_valid;
static char name_filter[BC_NAME_LEN];
static uint8_t user_code[BT_ISO_BROADCAST_CODE_SIZE];
static bool user_code_set;

/* Bike hub (giu lock) */
static const uint8_t bhs_uuid_bytes[] = {BHS_UUID_SVC_VAL};
static bt_addr_le_t hub_cand;    /* bo quang ba cua so ghep cua hub */
static atomic_t hub_found;       /* da bao EV_HUB_FOUND trong lan quet nay */
static uint8_t hub_info[BHS_JOIN_INFO_LEN];
static int hub_err;

/* Hub da ghep, luu "bcast/hub" */
struct bc_saved_hub {
	uint32_t broadcast_id;
	bt_addr_le_t addr;
	uint8_t sid;
	uint8_t code_len;
	uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
};
static struct bc_saved_hub saved;
static bool saved_valid;

/* ---------------- Trang thai (chi thread bo dieu khien) ---------------- */

static enum bc_mode mode;
static atomic_t mode_a;
static bool assisted; /* phien do dien thoai (Broadcast Assistant) yeu cau */
static struct bc_target tgt;
static struct bt_conn *past_conn; /* ref, khi dang dang ky nhan PAST */
static struct bt_le_per_adv_sync *pa;
static struct bt_bap_broadcast_sink *sink;
static struct bc_base_info base;
static bool base_ok;
static bool syncable;
static bool encrypted;
static bool code_ok;
static uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
static uint32_t want_bis[BC_MAX_SG];
static uint8_t want_nsg;
static bool bis_req_changed;
static uint32_t big_bits; /* BIS dang dong bo / dang xin dong bo */
static uint8_t retries;
static bool retry_wait;
static bool warned;
static bool big_logged;    /* da ghi thong so BIG cua phien nay */
static bool search_prompt; /* am "dang tim" (lap) dang phat */
static bool quiet;         /* phien tu vao lai: khong bao am that bai */
static bool hub_retried;   /* da xoa khoa cu + ghep lai mot lan trong lan bam nut nay */
static bool rejoin_enabled; /* co hub da ghep va nguoi dung chua roi broadcast bang nut */

/* ---------------- Luong BIS -> loa ---------------- */

struct bc_stream {
	struct bt_bap_stream stream;
	struct audio_stream_cfg cfg;
	bool started; /* BIS dang chay */
	bool out;     /* dang dua ra audio_out */
	uint32_t rx;
	uint32_t bad;
};

static struct bc_stream bcs[BC_STREAMS];
static struct bt_bap_stream *bcs_p[BC_STREAMS];
static K_MUTEX_DEFINE(out_lock);
static bool held; /* luong sink unicast dang chiem loa */

static void t_phase_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	post(EV_T_PHASE);
}

static void t_choose_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	post(EV_T_CHOOSE);
}

static void t_retry_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	post(EV_T_RETRY);
}

static K_TIMER_DEFINE(t_phase, t_phase_fn, NULL);
static K_TIMER_DEFINE(t_choose, t_choose_fn, NULL);
static K_TIMER_DEFINE(t_retry, t_retry_fn, NULL);

static void t_rejoin_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	post(EV_REJOIN);
}

static K_TIMER_DEFINE(t_rejoin, t_rejoin_fn, NULL);

#define BC_REJOIN_FIRST_S 5  /* sau khi bat may */
#define BC_REJOIN_RETRY_S 30 /* hub chua bat / ngoai tam: thu lai */

const char *bcast_mode_str(enum bc_mode m)
{
	switch (m) {
	case BC_OFF:
		return "OFF";
	case BC_SCAN:
		return "SCAN";
	case BC_HUB_LINK:
		return "HUB_LINK";
	case BC_PA_WAIT:
		return "PA_WAIT";
	case BC_PA_SYNCED:
		return "PA_SYNCED";
	case BC_BIG_SYNCING:
		return "BIG_SYNCING";
	case BC_STREAMING:
		return "STREAMING";
	default:
		return "?";
	}
}

enum bc_mode bcast_mode(void)
{
	return (enum bc_mode)atomic_get(&mode_a);
}

bool bcast_active(void)
{
	return atomic_get(&mode_a) != BC_OFF;
}

static void set_mode(enum bc_mode m)
{
	if (m == mode) {
		return;
	}
	LOG_INF("Auracast %s -> %s", bcast_mode_str(mode), bcast_mode_str(m));
	mode = m;
	atomic_set(&mode_a, m);
}

/* Goi khi giu out_lock */
static void out_update(struct bc_stream *s)
{
	const uint8_t idx = (uint8_t)(s - bcs);
	const bool want = s->started && !held;

	if (want == s->out) {
		return;
	}
	s->out = want;
	if (want) {
		audio_out_stream_start(idx, &s->cfg);
	} else {
		audio_out_stream_stop(idx);
	}
}

void bcast_audio_hold(bool hold)
{
	bool any = false;

	k_mutex_lock(&out_lock, K_FOREVER);
	if (hold != held) {
		held = hold;
		for (size_t i = 0; i < ARRAY_SIZE(bcs); i++) {
			any |= bcs[i].started;
			out_update(&bcs[i]);
		}
	}
	k_mutex_unlock(&out_lock);

	if (any) {
		LOG_INF("Auracast audio %s (unicast stream %s)", hold ? "paused" : "resumed",
			hold ? "started" : "ended");
	}
}

static void bc_stream_started(struct bt_bap_stream *stream)
{
	struct bc_stream *s = CONTAINER_OF(stream, struct bc_stream, stream);
	const uint8_t idx = (uint8_t)(s - bcs);
	struct bt_iso_info info;
	uint8_t bis = 0;
	int err;

	err = stream->codec_cfg ? audio_stream_cfg_parse_sink(stream->codec_cfg, &s->cfg) : -EINVAL;
	if (err) {
		LOG_ERR("BIS stream %u: unsupported codec config (%d)", idx, err);
		return;
	}
	if (stream->iso != NULL && bt_iso_chan_get_info(stream->iso, &info) == 0) {
		bis = info.sync_receiver.bis_number;
	}
	LOG_INF("BIS %u -> stream %u: LC3 %u Hz, %u us, %u ch (loc 0x%x), %u B/frame", bis, idx,
		s->cfg.freq_hz, s->cfg.frame_dur_us, s->cfg.chan_cnt, s->cfg.chan_alloc,
		s->cfg.octets_per_frame);

	k_mutex_lock(&out_lock, K_FOREVER);
	s->rx = 0;
	s->bad = 0;
	s->started = true;
	out_update(s);
	k_mutex_unlock(&out_lock);
}

static void bc_stream_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	struct bc_stream *s = CONTAINER_OF(stream, struct bc_stream, stream);

	k_mutex_lock(&out_lock, K_FOREVER);
	if (!s->started) {
		k_mutex_unlock(&out_lock);
		return;
	}
	s->started = false;
	out_update(s);
	k_mutex_unlock(&out_lock);

	LOG_INF("BIS stream %u stopped (reason 0x%02x, %u SDU, %u bad)", (uint8_t)(s - bcs),
		reason, s->rx, s->bad);
}

/* bt_rx (thread duy nhat goi audio_out_stream_data, cung voi le_audio.c / ic_audio.c) */
static void bc_stream_recv(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
			   struct net_buf *buf)
{
	struct bc_stream *s = CONTAINER_OF(stream, struct bc_stream, stream);
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0 && buf->len > 0;

	s->rx++;
	if (!valid) {
		s->bad++;
	}
	if (s->out) {
		audio_out_stream_data((uint8_t)(s - bcs), valid, info->ts, buf->data, buf->len);
	}
}

static struct bt_bap_stream_ops bc_stream_ops = {
	.started = bc_stream_started,
	.stopped = bc_stream_stopped,
	.recv = bc_stream_recv,
};

/* ---------------- Phan tich BASE (bt_rx) ---------------- */

struct parse_ctx {
	struct bc_base_info *out;
	struct bc_sg_info *sg;
	struct bt_audio_codec_cfg sg_cfg;
	bool sg_lc3;
};

static bool merge_cb(struct bt_data *data, void *user_data)
{
	return bt_audio_codec_cfg_set_val(user_data, data->type, data->data, data->data_len) >= 0;
}

/* Codec config cua BIS = cua subgroup, gia tri muc BIS (vd Audio Location) ghi de (BAP 3.7.2.2).
 * Bien tam static (chi bt_rx dung): bot stack cua thread bt_rx.
 */
static bool parse_bis_cb(const struct bt_bap_base_subgroup_bis *bis, void *user_data)
{
	static struct bt_audio_codec_cfg cfg;
	struct parse_ctx *c = user_data;
	struct bc_bis_info *b;

	if (c->sg->n_bis >= BC_MAX_BIS_SG) {
		return false;
	}
	b = &c->sg->bis[c->sg->n_bis++];
	b->index = bis->index;
	b->ok = false;
	if (!c->sg_lc3) {
		return true;
	}

	cfg = c->sg_cfg;
	if (bis->data_len > 0 && bt_audio_data_parse(bis->data, bis->data_len, merge_cb, &cfg) != 0) {
		return true;
	}
	b->ok = audio_stream_cfg_parse_sink(&cfg, &b->cfg) == 0;
	return true;
}

static bool parse_sg_cb(const struct bt_bap_base_subgroup *subgroup, void *user_data)
{
	struct parse_ctx *c = user_data;

	if (c->out->n_sg >= BC_MAX_SG) {
		return false;
	}
	c->sg = &c->out->sg[c->out->n_sg++];
	c->sg->n_bis = 0;
	c->sg_lc3 = bt_bap_base_subgroup_codec_to_codec_cfg(subgroup, &c->sg_cfg) == 0 &&
		    c->sg_cfg.id == BT_HCI_CODING_FORMAT_LC3;
	(void)bt_bap_base_subgroup_foreach_bis(subgroup, parse_bis_cb, c);
	return true;
}

/* ---------------- Callback Broadcast Sink (bt_rx) ---------------- */

/* Goi moi su kien PA co BASE: chi phan tich khi BASE doi */
static void bs_base_recv(struct bt_bap_broadcast_sink *s, const struct bt_bap_base *b,
			 size_t size)
{
	static struct bc_base_info info;
	static struct parse_ctx ctx;
	k_spinlock_key_t key;
	bool same;

	ARG_UNUSED(s);

	if (size > sizeof(base_raw)) {
		return;
	}
	key = k_spin_lock(&lock);
	same = size == base_raw_len && memcmp(base_raw, b, size) == 0;
	k_spin_unlock(&lock, key);
	if (same) {
		return;
	}

	memset(&info, 0, sizeof(info));
	memset(&ctx, 0, sizeof(ctx));
	ctx.out = &info;
	info.pd_us = bt_bap_base_get_pres_delay(b);
	(void)bt_bap_base_foreach_subgroup(b, parse_sg_cb, &ctx);

	key = k_spin_lock(&lock);
	memcpy(base_raw, b, size);
	base_raw_len = (uint16_t)size;
	base_new = info;
	k_spin_unlock(&lock, key);
	post(EV_BASE);
}

/* Goi moi su kien PA co BIGInfo (toi khi dong bo BIG): chi bao khi doi */
static void bs_syncable(struct bt_bap_broadcast_sink *s, const struct bt_iso_biginfo *biginfo)
{
	const atomic_val_t v = biginfo->encryption ? 2 : 1;
	k_spinlock_key_t key;

	ARG_UNUSED(s);

	if (atomic_set(&syncable_a, v) == v) {
		return;
	}
	key = k_spin_lock(&lock);
	enc_new = biginfo->encryption;
	big_new = (struct bc_big_params){
		.num_bis = biginfo->num_bis,
		.nse = biginfo->sub_evt_count,
		.iso_interval = biginfo->iso_interval,
		.bn = biginfo->burst_number,
		.pto = biginfo->offset,
		.irc = biginfo->rep_count,
		.max_pdu = biginfo->max_pdu,
		.sdu_interval = biginfo->sdu_interval,
		.max_sdu = biginfo->max_sdu,
		.phy = biginfo->phy,
		.framing = biginfo->framing,
	};
	k_spin_unlock(&lock, key);
	post(EV_SYNCABLE);
}

static void bs_started(struct bt_bap_broadcast_sink *s)
{
	ARG_UNUSED(s);
	post(EV_BIG_STARTED);
}

/* Co the chay ngay trong bt_bap_broadcast_sink_stop() (thread bo dieu khien) */
static void bs_stopped(struct bt_bap_broadcast_sink *s, uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(s);
	big_stop_reason = reason;
	k_spin_unlock(&lock, key);
	post(EV_BIG_STOPPED);
}

static struct bt_bap_broadcast_sink_cb sink_cbs = {
	.base_recv = bs_base_recv,
	.syncable = bs_syncable,
	.started = bs_started,
	.stopped = bs_stopped,
};

/* ---------------- Callback PA sync (bt_rx) ---------------- */

static void pa_synced_cb(struct bt_le_per_adv_sync *sync,
			 struct bt_le_per_adv_sync_synced_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const bool ours = info->sid == want_sid && ((want_past && info->conn != NULL) ||
						    (want_create && info->conn == NULL));

	if (ours) {
		synced_pa = sync;
		cur_pa = sync;
		want_past = false;
		want_create = false;
	}
	k_spin_unlock(&lock, key);

	if (ours) {
		post(EV_PA_SYNCED);
	}
}

static void pa_term_cb(struct bt_le_per_adv_sync *sync,
		       const struct bt_le_per_adv_sync_term_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const bool ours = sync == cur_pa;

	if (ours) {
		cur_pa = NULL;
		pa_lost_reason = info->reason;
	}
	k_spin_unlock(&lock, key);

	if (ours) {
		post(EV_PA_LOST);
	}
}

static struct bt_le_per_adv_sync_cb pa_cbs = {
	.synced = pa_synced_cb,
	.term = pa_term_cb,
};

/* ---------------- Callback Scan Delegator: lenh tu dien thoai (bt_rx) ---------------- */

static bool busy_elsewhere(void)
{
	return intercom_mode() != ICM_NORMAL || app_state() == HS_SHUTDOWN;
}

static int sd_pa_sync_req(struct bt_conn *conn,
			  const struct bt_bap_scan_delegator_recv_state *rs, bool past_avail,
			  uint16_t pa_interval)
{
	struct bt_conn *old;
	k_spinlock_key_t key;

	if (busy_elsewhere()) {
		LOG_WRN("Phone asked to join broadcast 0x%06x - rejected (intercom active)",
			rs->broadcast_id);
		return -EBUSY;
	}

	key = k_spin_lock(&lock);
	memset(&req_tgt, 0, sizeof(req_tgt));
	bt_addr_le_copy(&req_tgt.addr, &rs->addr);
	req_tgt.sid = rs->adv_sid;
	req_tgt.broadcast_id = rs->broadcast_id;
	req_tgt.pa_interval = pa_interval;
	req_tgt.src_id = rs->src_id;
	req_tgt.src_valid = true;
	req_past = past_avail && conn != NULL;
	old = req_conn;
	req_conn = conn ? bt_conn_ref(conn) : NULL;
	k_spin_unlock(&lock, key);
	if (old != NULL) {
		bt_conn_unref(old);
	}

	LOG_INF("Phone (assistant): join broadcast 0x%06x, SID %u, src %u, PAST %s",
		rs->broadcast_id, rs->adv_sid, rs->src_id, past_avail ? "yes" : "no");
	post(EV_PA_REQ);
	return 0;
}

static int sd_pa_sync_term_req(struct bt_conn *conn,
			       const struct bt_bap_scan_delegator_recv_state *rs)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(conn);
	term_src_id = rs->src_id;
	k_spin_unlock(&lock, key);
	LOG_INF("Phone (assistant): stop PA sync (src %u)", rs->src_id);
	post(EV_PA_TERM);
	return 0;
}

static void sd_broadcast_code(struct bt_conn *conn,
			      const struct bt_bap_scan_delegator_recv_state *rs,
			      const uint8_t broadcast_code[BT_ISO_BROADCAST_CODE_SIZE])
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(conn);
	memcpy(req_code, broadcast_code, sizeof(req_code));
	k_spin_unlock(&lock, key);
	LOG_INF("Phone (assistant): broadcast code received (src %u)", rs->src_id);
	post(EV_CODE);
}

static int sd_bis_sync_req(struct bt_conn *conn,
			   const struct bt_bap_scan_delegator_recv_state *rs,
			   const uint32_t bis_sync_req[BT_BAP_BASS_MAX_SUBGROUPS])
{
	const uint8_t n = MIN(rs->num_subgroups, BC_MAX_SG);
	k_spinlock_key_t key;
	bool any = false;

	ARG_UNUSED(conn);

	for (uint8_t i = 0; i < n; i++) {
		any |= bis_sync_req[i] != 0U;
	}
	if (any && busy_elsewhere()) {
		LOG_WRN("Phone asked for BIS sync - rejected (intercom active)");
		return -EBUSY;
	}

	key = k_spin_lock(&lock);
	req_nsg = n;
	memset(req_bis, 0, sizeof(req_bis));
	memcpy(req_bis, bis_sync_req, n * sizeof(req_bis[0]));
	k_spin_unlock(&lock, key);

	LOG_INF("Phone (assistant): BIS sync request src %u: 0x%08x 0x%08x", rs->src_id,
		n > 0 ? bis_sync_req[0] : 0U, n > 1 ? bis_sync_req[1] : 0U);
	post(EV_BIS_REQ);
	return 0;
}

static int sd_remove_source(struct bt_conn *conn, uint8_t src_id)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(conn);
	removed_src_id = src_id;
	k_spin_unlock(&lock, key);
	LOG_INF("Phone (assistant): remove source %u", src_id);
	post(EV_SRC_REMOVED);
	return 0;
}

static void sd_recv_state_updated(struct bt_conn *conn,
				  const struct bt_bap_scan_delegator_recv_state *rs)
{
	ARG_UNUSED(conn);
	LOG_DBG("Receive state src %u: PA %u, enc %u, BIS 0x%08x", rs->src_id, rs->pa_sync_state,
		rs->encrypt_state, rs->num_subgroups ? rs->subgroups[0].bis_sync : 0U);
}

static struct bt_bap_scan_delegator_cb sd_cbs = {
	.recv_state_updated = sd_recv_state_updated,
	.pa_sync_req = sd_pa_sync_req,
	.pa_sync_term_req = sd_pa_sync_term_req,
	.broadcast_code = sd_broadcast_code,
	.bis_sync_req = sd_bis_sync_req,
	.remove_source = sd_remove_source,
};

/* ---------------- Tu tim broadcast (bt_rx) ---------------- */

/* str chua sub, khong phan biet hoa thuong (ASCII) */
static bool contains_ci(const char *str, const char *sub)
{
	const size_t n = strlen(sub);

	if (n == 0) {
		return true;
	}
	for (const char *p = str; *p != '\0'; p++) {
		size_t i = 0;

		while (i < n && p[i] != '\0' &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)sub[i])) {
			i++;
		}
		if (i == n) {
			return true;
		}
	}
	return false;
}

struct scan_parse {
	bool found;
	bool bname;
	bool hub; /* Bike hub dang mo cua so ghep */
	uint32_t broadcast_id;
	char name[BC_NAME_LEN];
};

static void copy_name(char *dst, const struct bt_data *data)
{
	const size_t n = MIN(data->data_len, BC_NAME_LEN - 1);

	memcpy(dst, data->data, n);
	dst[n] = '\0';
}

static bool scan_ad_cb(struct bt_data *data, void *user_data)
{
	struct scan_parse *p = user_data;

	switch (data->type) {
	case BT_DATA_SVC_DATA16:
		/* Broadcast Audio Announcement: UUID 0x1852 + Broadcast ID 3 byte */
		if (data->data_len >= BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE &&
		    sys_get_le16(data->data) == BT_UUID_BROADCAST_AUDIO_VAL) {
			p->broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);
			p->found = true;
		}
		break;
	case BT_DATA_SVC_DATA128:
		/* Bike hub: {BHS UUID, ver, co} - co "dang ghep" */
		if (data->data_len >= sizeof(bhs_uuid_bytes) + 2 &&
		    memcmp(data->data, bhs_uuid_bytes, sizeof(bhs_uuid_bytes)) == 0 &&
		    (data->data[sizeof(bhs_uuid_bytes) + 1] & BHS_FLAG_PAIRING)) {
			p->hub = true;
		}
		break;
	case BT_DATA_BROADCAST_NAME:
		copy_name(p->name, data);
		p->bname = true;
		break;
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
		if (!p->bname) {
			copy_name(p->name, data);
		}
		break;
	default:
		break;
	}
	return true;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	struct net_buf_simple_state st;
	struct scan_parse p = {0};
	k_spinlock_key_t key;
	bool first;

	if (atomic_get(&mode_a) != BC_SCAN) {
		return;
	}
	net_buf_simple_save(ad, &st);
	bt_data_parse(ad, scan_ad_cb, &p);
	net_buf_simple_restore(ad, &st);

	/* Bike hub dang mo cua so ghep (bo quang ba co the ket noi): uu tien hon moi broadcast */
	if (p.hub && (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) {
		if (!atomic_set(&hub_found, 1)) {
			key = k_spin_lock(&lock);
			bt_addr_le_copy(&hub_cand, info->addr);
			k_spin_unlock(&lock, key);
			post(EV_HUB_FOUND);
		}
		return;
	}

	/* Chi bo quang ba co periodic advertising (interval != 0) moi co the la broadcast */
	if (info->interval == 0U || !p.found) {
		return;
	}

	key = k_spin_lock(&lock);
	if (name_filter[0] != '\0' && !contains_ci(p.name, name_filter)) {
		k_spin_unlock(&lock, key);
		return;
	}
	first = !cand_valid;
	if (first || info->rssi > cand_rssi) {
		memset(&cand, 0, sizeof(cand));
		bt_addr_le_copy(&cand.addr, info->addr);
		cand.sid = info->sid;
		cand.broadcast_id = p.broadcast_id;
		cand.pa_interval = info->interval;
		memcpy(cand.name, p.name, sizeof(cand.name));
		cand_rssi = info->rssi;
		cand_valid = true;
	}
	k_spin_unlock(&lock, key);

	if (first) {
		LOG_INF("Found broadcast \"%s\" (ID 0x%06x, RSSI %d) - choosing in %d ms", p.name,
			p.broadcast_id, info->rssi, BC_CHOOSE_MS);
		k_timer_start(&t_choose, K_MSEC(BC_CHOOSE_MS), K_NO_WAIT);
	}
}

static struct bt_le_scan_cb scan_cbs = {
	.recv = scan_recv,
};

/* ---------------- Dieu khien (thread bo dieu khien) ---------------- */

static uint16_t pa_timeout_10ms(uint16_t interval)
{
	uint32_t t;

	if (interval == BT_BAP_PA_INTERVAL_UNKNOWN || interval == 0U) {
		return 1000; /* 10 s */
	}
	/* 4 lan khoang cach giua hai lan nghe (interval x (skip + 1)), toi thieu 2 s */
	t = BT_GAP_PER_ADV_INTERVAL_TO_US(interval) * (BC_PA_SKIP + 1U) * 4U / 10000U;
	return (uint16_t)CLAMP(t, 200U, BT_GAP_PER_ADV_MAX_TIMEOUT);
}

static bool same_source(const struct bc_target *a, const struct bc_target *b)
{
	/* BAP 6.5.4: kieu dia chi + SID + Broadcast ID (dia chi co the la RPA da phan giai) */
	return a->addr.type == b->addr.type && a->sid == b->sid &&
	       a->broadcast_id == b->broadcast_id;
}

static void stop_timers(void)
{
	k_timer_stop(&t_phase);
	k_timer_stop(&t_choose);
	k_timer_stop(&t_retry);
	retry_wait = false;
}

static void reset_media(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	base_raw_len = 0;
	k_spin_unlock(&lock, key);
	atomic_set(&syncable_a, 0);
	base_ok = false;
	syncable = false;
	encrypted = false;
	big_bits = 0;
	warned = false;
	big_logged = false;
}

static bool find_local_src_cb(const struct bt_bap_scan_delegator_recv_state *rs, void *user_data)
{
	const struct bc_target *t = user_data;

	return rs->broadcast_id == t->broadcast_id && rs->adv_sid == t->sid;
}

/* Dung BIG, xoa PA va broadcast sink. Giu nguyen tgt / assisted. */
static void teardown(void)
{
	struct bt_le_per_adv_sync *late;
	k_spinlock_key_t key;
	int err;

	stop_timers();
	if (mode == BC_SCAN) {
		(void)bt_le_scan_stop();
	}
	if (mode == BC_HUB_LINK) {
		hub_link_cancel();
	}

	key = k_spin_lock(&lock);
	want_create = false;
	want_past = false;
	cur_pa = NULL; /* lan ket thuc PA ben duoi la do minh: pa_term_cb bo qua */
	late = synced_pa;
	synced_pa = NULL;
	k_spin_unlock(&lock, key);

	if (past_conn != NULL) {
		(void)bt_le_per_adv_sync_transfer_unsubscribe(past_conn);
		bt_conn_unref(past_conn);
		past_conn = NULL;
	}
	/* Dung BIG truoc (callback stopped chay ngay tai day), roi PA (thu vien cap nhat
	 * PA state cua BASS khi sink con ton tai), cuoi cung xoa sink.
	 */
	if (sink != NULL && (mode == BC_BIG_SYNCING || mode == BC_STREAMING)) {
		err = bt_bap_broadcast_sink_stop(sink);
		if (err && err != -EALREADY) {
			LOG_WRN("BIG stop failed (%d)", err);
		}
	}
	if (late != NULL && late != pa) {
		(void)bt_le_per_adv_sync_delete(late);
	}
	if (pa != NULL) {
		err = bt_le_per_adv_sync_delete(pa);
		if (err) {
			LOG_WRN("PA sync delete failed (%d)", err);
		}
		pa = NULL;
	}
	if (sink != NULL) {
		err = bt_bap_broadcast_sink_delete(sink);
		if (err) {
			LOG_WRN("Broadcast sink delete failed (%d)", err);
		}
		sink = NULL;
	}
	/* Tu tim: Receive State do thu vien tu them -> xoa de giai phong o */
	if (!assisted && tgt.src_valid) {
		(void)bt_bap_scan_delegator_rem_src(tgt.src_id);
		tgt.src_valid = false;
	}
	reset_media();
}

static void end_session(enum prompt_id p, bool play)
{
	teardown();
	assisted = false;
	retries = 0;
	memset(want_bis, 0, sizeof(want_bis));
	want_nsg = 0;
	set_mode(BC_OFF);
	if (quiet && p == PROMPT_BC_FAIL) {
		play = false; /* tu vao lai that bai (hub chua bat): im lang, thu lai sau */
	}
	if (play) {
		audio_out_prompt(p);
	} else if (search_prompt) {
		audio_out_prompt_stop(); /* chi dung am "dang tim" dang lap, khong cat am khac */
	}
	search_prompt = false;
	quiet = false;
	if (rejoin_enabled && saved_valid) {
		k_timer_start(&t_rejoin, K_SECONDS(BC_REJOIN_RETRY_S), K_NO_WAIT);
	}
	power_activity(); /* het nghe: hen gio tu tat chay lai (neu dang IDLE) */
}

static int pa_create(void)
{
	struct bt_le_per_adv_sync_param p = {0};
	struct bt_le_local_features f;
	k_spinlock_key_t key;
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_copy(&p.addr, &tgt.addr);
	p.sid = tgt.sid;
	p.skip = BC_PA_SKIP;
	p.timeout = pa_timeout_10ms(tgt.pa_interval);
	/* Bo qua bao cao PA trung (ADI): BASE lap lai khong lam ton CPU */
	p.options = (bt_le_get_local_features(&f) == 0 && BT_FEAT_LE_PER_ADV_ADI_SUPP(f.features))
			    ? BT_LE_PER_ADV_SYNC_OPT_FILTER_DUPLICATE
			    : BT_LE_PER_ADV_SYNC_OPT_NONE;

	key = k_spin_lock(&lock);
	want_create = true;
	want_sid = tgt.sid;
	k_spin_unlock(&lock, key);

	/* Host tu bat quet khi co dong bo PA dang cho (scan.c: BT_LE_SCAN_USER_PER_SYNC) */
	err = bt_le_per_adv_sync_create(&p, &pa);

	key = k_spin_lock(&lock);
	if (err) {
		want_create = false;
		pa = NULL;
	} else {
		cur_pa = pa;
	}
	k_spin_unlock(&lock, key);

	if (err) {
		LOG_ERR("PA sync create failed (%d)", err);
		return err;
	}

	bt_addr_le_to_str(&tgt.addr, addr, sizeof(addr));
	LOG_INF("Syncing to PA of %s SID %u (timeout %u0 ms)", addr, tgt.sid, p.timeout);
	set_mode(BC_PA_WAIT);
	k_timer_start(&t_phase, K_SECONDS(BC_PA_TIMEOUT_S), K_NO_WAIT);
	return 0;
}

/* Dien thoai gui thong tin dong bo PA qua ket noi ACL (PAST): khong phai quet */
static int past_start(struct bt_conn *conn)
{
	const struct bt_le_per_adv_sync_transfer_param p = {
		.skip = BC_PA_SKIP,
		.timeout = pa_timeout_10ms(tgt.pa_interval),
		.options = BT_LE_PER_ADV_SYNC_TRANSFER_OPT_NONE,
	};
	k_spinlock_key_t key;
	int err;

	key = k_spin_lock(&lock);
	want_past = true;
	want_sid = tgt.sid;
	k_spin_unlock(&lock, key);

	err = bt_le_per_adv_sync_transfer_subscribe(conn, &p);
	if (err) {
		key = k_spin_lock(&lock);
		want_past = false;
		k_spin_unlock(&lock, key);
		LOG_WRN("PAST subscribe failed (%d) - will scan instead", err);
		return err;
	}
	past_conn = bt_conn_ref(conn);

	/* Bao dien thoai "SyncInfo Request": dien thoai gui PAST khi thay trang thai nay */
	(void)bt_bap_scan_delegator_set_pa_state(tgt.src_id, BT_BAP_PA_STATE_INFO_REQ);
	LOG_INF("Waiting for PAST from phone (SID %u)", tgt.sid);
	set_mode(BC_PA_WAIT);
	k_timer_start(&t_phase, K_SECONDS(BC_PA_TIMEOUT_S), K_NO_WAIT);
	return 0;
}

/* Bao "Failed" trong Receive State sau khi da xoa PA (xoa PA lam thu vien dat "Not synced") */
static void end_failed(bool play)
{
	const bool report = assisted && tgt.src_valid;

	end_session(PROMPT_BC_FAIL, play);
	if (report) {
		(void)bt_bap_scan_delegator_set_pa_state(tgt.src_id, BT_BAP_PA_STATE_FAILED);
	}
}

static void give_up(void)
{
	LOG_WRN("Giving up on broadcast 0x%06x", tgt.broadcast_id);
	end_failed(true);
}

/* Mat PA / BIG hoac dong bo PA that bai: lam lai tu dau (tu tao PA), toi da BC_MAX_RETRY lan */
static void retry_session(void)
{
	teardown();
	if (++retries > BC_MAX_RETRY) {
		give_up();
		return;
	}
	LOG_INF("Retrying broadcast 0x%06x (%u/%u)", tgt.broadcast_id, retries, BC_MAX_RETRY);
	if (pa_create() != 0) {
		give_up();
	}
}

static void start_scan(void)
{
	static const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0060, /* 60 ms */
		.window = 0x0030,   /* 30 ms: chua thoi gian radio cho dien thoai */
	};
	const enum helmet_state s = app_state();
	k_spinlock_key_t key;
	int err;

	if (intercom_mode() != ICM_NORMAL) {
		LOG_WRN("Auracast search not available during intercom");
		audio_out_prompt(PROMPT_BC_FAIL);
		return;
	}
	if (s == HS_RINGING || s == HS_IN_CALL || s == HS_SHUTDOWN || s == HS_BOOT) {
		LOG_WRN("Auracast search not available in state %s", app_state_str(s));
		return;
	}

	assisted = false;
	retries = 0;
	reset_media();
	memset(&tgt, 0, sizeof(tgt));

	key = k_spin_lock(&lock);
	cand_valid = false;
	code_ok = user_code_set;
	memcpy(code, user_code, sizeof(code));
	k_spin_unlock(&lock, key);
	atomic_set(&hub_found, 0);
	quiet = false;
	k_timer_stop(&t_rejoin);

	set_mode(BC_SCAN); /* truoc khi quet: scan_recv kiem tra mode_a */
	err = bt_le_scan_start(&param, NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("Scan start failed (%d)", err);
		set_mode(BC_OFF);
		audio_out_prompt(PROMPT_BC_FAIL);
		return;
	}
	k_timer_start(&t_phase, K_SECONDS(BC_SCAN_TIMEOUT_S), K_NO_WAIT);
	LOG_INF("Auracast: searching for broadcasts%s%s%s (%d s)",
		name_filter[0] ? " named \"" : "", name_filter, name_filter[0] ? "\"" : "",
		BC_SCAN_TIMEOUT_S);
	audio_out_prompt(PROMPT_BC_SEARCH);
	search_prompt = true;
}

static void on_pa_req(void)
{
	struct bc_target t;
	struct bt_conn *conn;
	k_spinlock_key_t key;
	bool past;

	key = k_spin_lock(&lock);
	t = req_tgt;
	past = req_past;
	conn = req_conn;
	req_conn = NULL;
	k_spin_unlock(&lock, key);

	if (assisted && mode != BC_OFF && same_source(&t, &tgt) &&
	    (mode == BC_PA_WAIT || pa != NULL)) {
		LOG_INF("Already syncing to broadcast 0x%06x", t.broadcast_id);
		tgt.src_id = t.src_id;
		tgt.src_valid = true;
		goto out;
	}

	if (mode != BC_OFF) {
		LOG_INF("Switching to the broadcast requested by the phone");
		teardown();
		if (search_prompt) {
			audio_out_prompt_stop();
			search_prompt = false;
		}
	}

	assisted = true;
	tgt = t;
	retries = 0;
	code_ok = false;
	memset(want_bis, 0, sizeof(want_bis));
	want_nsg = 0;

	if (past && conn != NULL && past_start(conn) == 0) {
		goto out;
	}
	if (pa_create() != 0) {
		end_failed(false);
	}
out:
	if (conn != NULL) {
		bt_conn_unref(conn);
	}
}

static void on_session_end_req(atomic_val_t ev)
{
	k_spinlock_key_t key;
	uint8_t term_id;
	uint8_t rem_id;

	key = k_spin_lock(&lock);
	term_id = term_src_id;
	rem_id = removed_src_id;
	k_spin_unlock(&lock, key);

	if (mode == BC_OFF || !tgt.src_valid) {
		return;
	}
	/* Ca nguon tu tim (Receive State cuc bo): dien thoai noi voi mu cung duoc dung no */
	if (((ev & BIT(EV_PA_TERM)) && term_id == tgt.src_id) ||
	    ((ev & BIT(EV_SRC_REMOVED)) && rem_id == tgt.src_id)) {
		LOG_INF("Phone ended broadcast 0x%06x for this helmet", tgt.broadcast_id);
		end_session(PROMPT_BC_LEFT, mode == BC_STREAMING);
	}
}

static void on_scan_choose(void)
{
	char addr[BT_ADDR_LE_STR_LEN];
	k_spinlock_key_t key;
	bool valid;
	int8_t rssi;

	if (mode != BC_SCAN) {
		return;
	}
	(void)bt_le_scan_stop();
	k_timer_stop(&t_phase);

	key = k_spin_lock(&lock);
	valid = cand_valid;
	tgt = cand;
	rssi = cand_rssi;
	k_spin_unlock(&lock, key);
	if (!valid) {
		end_session(PROMPT_BC_FAIL, true);
		return;
	}

	if (saved_valid && tgt.broadcast_id == saved.broadcast_id && saved.code_len > 0) {
		/* Broadcast cua hub da ghep: dung ma da luu, khong can go */
		memcpy(code, saved.code, sizeof(code));
		code_ok = true;
	}
	bt_addr_le_to_str(&tgt.addr, addr, sizeof(addr));
	LOG_INF("Joining broadcast \"%s\" (ID 0x%06x, %s SID %u, RSSI %d)", tgt.name,
		tgt.broadcast_id, addr, tgt.sid, rssi);
	if (pa_create() != 0) {
		end_session(PROMPT_BC_FAIL, true);
	}
}

/* ---------------- Bike hub ---------------- */

/* Goi tu hub_link.c (bt_rx) */
void bcast_hub_result(int err, const uint8_t *info, uint16_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	hub_err = err;
	memset(hub_info, 0, sizeof(hub_info));
	if (err == 0 && info != NULL) {
		memcpy(hub_info, info, MIN(len, sizeof(hub_info)));
	}
	k_spin_unlock(&lock, key);
	post(EV_HUB_RESULT);
}

static void on_hub_found(void)
{
	char str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_t addr;
	k_spinlock_key_t key;
	int err;

	if (mode != BC_SCAN) {
		return;
	}
	(void)bt_le_scan_stop();
	k_timer_stop(&t_choose);
	key = k_spin_lock(&lock);
	bt_addr_le_copy(&addr, &hub_cand);
	k_spin_unlock(&lock, key);

	bt_addr_le_to_str(&addr, str, sizeof(str));
	LOG_INF("Bike hub %s is pairing - getting broadcast info", str);
	set_mode(BC_HUB_LINK);
	err = hub_link_start(&addr);
	if (err) {
		LOG_WRN("Hub link start failed (%d)", err);
		end_session(PROMPT_BC_FAIL, true);
	}
}

/* Bat dau dong bo broadcast cua hub (sau khi ghep, hoac tu vao lai) */
static int join_saved_hub(void)
{
	assisted = false;
	retries = 0;
	reset_media();
	memset(&tgt, 0, sizeof(tgt));
	bt_addr_le_copy(&tgt.addr, &saved.addr);
	tgt.sid = saved.sid;
	tgt.broadcast_id = saved.broadcast_id;
	tgt.pa_interval = BT_BAP_PA_INTERVAL_UNKNOWN;
	strncpy(tgt.name, "Bike Audio", sizeof(tgt.name) - 1);
	code_ok = saved.code_len > 0;
	memset(code, 0, sizeof(code));
	memcpy(code, saved.code, saved.code_len);
	return pa_create();
}

static void on_hub_result(void)
{
	struct bc_saved_hub h = {0};
	k_spinlock_key_t key;
	uint8_t info[BHS_JOIN_INFO_LEN];
	int err;

	if (mode != BC_HUB_LINK) {
		return;
	}
	key = k_spin_lock(&lock);
	err = hub_err;
	memcpy(info, hub_info, sizeof(info));
	k_spin_unlock(&lock, key);

	if (err == 0 && (info[0] != BHS_PROTO_VER || info[12] > BT_ISO_BROADCAST_CODE_SIZE)) {
		err = -EPROTO;
	}
	if (err == -EAGAIN && !hub_retried) {
		bt_addr_le_t stale;

		/* Khoa ghep doi cu lech voi hub: xoa dung hub do, quet + ghep lai (cua so con mo) */
		hub_retried = true;
		if (hub_link_stale_peer(&stale)) {
			char str[BT_ADDR_LE_STR_LEN];

			bt_addr_le_to_str(&stale, str, sizeof(str));
			LOG_INF("Stale bond with bike hub %s - deleting it and pairing again", str);
			(void)bt_unpair(BT_ID_DEFAULT, &stale);
		}
		teardown();
		set_mode(BC_OFF);
		start_scan();
		return;
	}
	if (err) {
		LOG_WRN("Pairing with bike hub failed (%d)", err);
		end_session(PROMPT_BC_FAIL, true);
		return;
	}

	h.broadcast_id = sys_get_le24(&info[1]);
	h.addr.type = info[4];
	memcpy(h.addr.a.val, &info[5], sizeof(h.addr.a.val));
	h.sid = info[11];
	h.code_len = info[12];
	memcpy(h.code, &info[13], h.code_len);
	saved = h;
	saved_valid = true;
	rejoin_enabled = true;
	(void)settings_save_one("bcast/hub", &saved, sizeof(saved));
	LOG_INF("Paired with bike hub: broadcast 0x%06x, SID %u, %s - saved", h.broadcast_id, h.sid,
		h.code_len ? "encrypted" : "open");

	/* Hub_link tu ngat; vao broadcast ngay (PA theo dia chi + SID, khong can quet) */
	if (join_saved_hub() != 0) {
		end_session(PROMPT_BC_FAIL, true);
	}
}

/* Hen gio: tu vao lai broadcast cua hub da ghep (bat may, hub tat roi bat lai, ra ngoai tam) */
static void on_rejoin(void)
{
	const enum helmet_state s = app_state();

	if (!saved_valid || !rejoin_enabled || mode != BC_OFF) {
		return;
	}
	/* Dang goi / intercom / dang nghe nhac unicast tu dien thoai: chua quet (tranh tranh radio) */
	if (intercom_mode() != ICM_NORMAL || s == HS_RINGING || s == HS_IN_CALL ||
	    s == HS_SHUTDOWN || s == HS_BOOT || le_audio_is_streaming()) {
		k_timer_start(&t_rejoin, K_SECONDS(BC_REJOIN_RETRY_S), K_NO_WAIT);
		return;
	}
	LOG_INF("Auto-joining paired bike hub broadcast 0x%06x", saved.broadcast_id);
	if (join_saved_hub() == 0) {
		quiet = true; /* hub chua bat thi that bai im lang, thu lai sau */
	} else {
		k_timer_start(&t_rejoin, K_SECONDS(BC_REJOIN_RETRY_S), K_NO_WAIT);
	}
}

static void on_pa_synced(void)
{
	const struct bt_bap_scan_delegator_recv_state *rs;
	struct bt_le_per_adv_sync_info info;
	struct bt_le_per_adv_sync *s;
	k_spinlock_key_t key;
	int err;

	key = k_spin_lock(&lock);
	s = synced_pa;
	synced_pa = NULL;
	k_spin_unlock(&lock, key);

	if (s == NULL) {
		return;
	}
	if (mode != BC_PA_WAIT) {
		if (s != pa) {
			(void)bt_le_per_adv_sync_delete(s); /* den tre, phien da huy */
		}
		return;
	}

	k_timer_stop(&t_phase);
	if (past_conn != NULL) {
		(void)bt_le_per_adv_sync_transfer_unsubscribe(past_conn);
		bt_conn_unref(past_conn);
		past_conn = NULL;
	}
	pa = s;
	if (bt_le_per_adv_sync_get_info(pa, &info) == 0) {
		LOG_INF("PA synced: interval %u ms, PHY %u", BT_GAP_PER_ADV_INTERVAL_TO_MS(info.interval),
			info.phy);
	}

	/* Thu vien tim Receive State khop (dien thoai yeu cau) hoac tu them moi (tu tim) */
	err = bt_bap_broadcast_sink_create(pa, tgt.broadcast_id, &sink);
	if (err) {
		LOG_ERR("Broadcast sink create failed (%d)", err);
		sink = NULL;
		retry_session();
		return;
	}
	if (!tgt.src_valid) {
		rs = bt_bap_scan_delegator_find_state(find_local_src_cb, &tgt);
		if (rs != NULL) {
			tgt.src_id = rs->src_id;
			tgt.src_valid = true;
		}
	}
	set_mode(BC_PA_SYNCED);
	if (!assisted) {
		k_timer_start(&t_phase, K_SECONDS(BC_SETUP_TIMEOUT_S), K_NO_WAIT);
	}
}

static void on_pa_lost(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint8_t r = pa_lost_reason;

	k_spin_unlock(&lock, key);

	if (mode == BC_OFF || mode == BC_SCAN) {
		return;
	}
	pa = NULL; /* host da giai phong doi tuong PA */
	if (mode == BC_STREAMING) {
		/* BIG khong can PA de tiep tuc: giu am thanh, chi lam lai khi BIG cung mat */
		LOG_WRN("PA sync lost (0x%02x) - BIG still running", r);
		return;
	}
	LOG_WRN("PA sync %s (0x%02x %s)", mode == BC_PA_WAIT ? "failed" : "lost", r,
		bt_hci_err_to_str(r));
	retry_session();
}

static void on_big_started(void)
{
	if (mode != BC_BIG_SYNCING) {
		return;
	}
	k_timer_stop(&t_phase);
	retries = 0;
	set_mode(BC_STREAMING);
	LOG_INF("Receiving broadcast \"%s\" 0x%06x: BIS 0x%08x", tgt.name, tgt.broadcast_id,
		big_bits);
	audio_out_prompt(PROMPT_BC_JOINED);
	search_prompt = false;
}

static void on_big_stopped(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const uint8_t r = big_stop_reason;
	const enum bc_mode was = mode;

	k_spin_unlock(&lock, key);

	/* Dung do minh (teardown / stop_big) da doi mode truoc -> khong vao day */
	if ((was != BC_BIG_SYNCING && was != BC_STREAMING) || r == BT_HCI_ERR_LOCALHOST_TERM_CONN) {
		return;
	}
	big_bits = 0;
	set_mode(BC_PA_SYNCED);
	if (was == BC_STREAMING) {
		power_activity(); /* het tieng: hen gio tu tat tinh lai tu luc nay */
	}

	if (r == BT_HCI_ERR_TERM_DUE_TO_MIC_FAIL) {
		LOG_WRN("Wrong broadcast code (BIG MIC failure)");
		code_ok = false;
		warned = false;
		if (!assisted) {
			/* Tu tim: cho ma moi ("helmet bc code"), khong roi broadcast */
			audio_out_prompt(PROMPT_BC_FAIL);
		}
		return; /* dien thoai thay "Bad Code" trong Receive State va gui ma moi */
	}

	if (pa == NULL) {
		LOG_WRN("BIG %s (reason 0x%02x %s), PA already lost",
			was == BC_STREAMING ? "lost" : "sync failed", r, bt_hci_err_to_str(r));
		retry_session();
		return;
	}

	/* PA con: chi dong bo lai khi co BIGInfo MOI. Nguon tam dung (dung nhac) thi BIGInfo
	 * bien mat toi khi nguon phat lai -> mu cho, khong roi broadcast.
	 */
	syncable = false;
	atomic_set(&syncable_a, 0);
	big_logged = false;
	if (r == BT_HCI_ERR_REMOTE_USER_TERM_CONN) {
		LOG_INF("Broadcaster ended the BIG (paused?) - waiting for it to resume");
		retries = 0;
		return;
	}
	LOG_WRN("BIG %s (reason 0x%02x %s) - resync on next BIGInfo",
		was == BC_STREAMING ? "lost" : "sync failed", r, bt_hci_err_to_str(r));
	if (++retries > BC_MAX_RETRY) {
		give_up();
		return;
	}
	retry_wait = true;
	k_timer_start(&t_retry, K_MSEC(300U * retries), K_NO_WAIT);
}

static void on_phase_timeout(void)
{
	switch (mode) {
	case BC_SCAN:
		LOG_WRN("No Auracast broadcast found in %d s", BC_SCAN_TIMEOUT_S);
		end_session(PROMPT_BC_FAIL, true);
		break;
	case BC_PA_WAIT:
		if (past_conn != NULL) {
			k_spinlock_key_t key = k_spin_lock(&lock);

			want_past = false;
			k_spin_unlock(&lock, key);
			LOG_WRN("No PAST from phone - syncing by scanning");
			(void)bt_le_per_adv_sync_transfer_unsubscribe(past_conn);
			bt_conn_unref(past_conn);
			past_conn = NULL;
			if (tgt.src_valid) {
				(void)bt_bap_scan_delegator_set_pa_state(tgt.src_id,
									 BT_BAP_PA_STATE_NO_PAST);
			}
			if (pa_create() != 0) {
				give_up();
			}
		} else {
			LOG_WRN("PA sync timeout");
			retry_session();
		}
		break;
	case BC_PA_SYNCED:
		if (assisted) {
			break;
		}
		if (!base_ok) {
			LOG_WRN("Cannot join broadcast: no BASE in %d s", BC_SETUP_TIMEOUT_S);
			end_session(PROMPT_BC_FAIL, true);
			break;
		}
		/* Co PA + BASE: cho tiep khong gioi han (roi bang nut / "helmet bc stop") */
		LOG_WRN("Still waiting after %d s: %s", BC_SETUP_TIMEOUT_S,
			!syncable ? "no BIGInfo - is the source playing audio?"
			: (encrypted && !code_ok)
				? "encrypted - set the code with: helmet bc code <text>"
				: "no playable BIS");
		break;
	default:
		break;
	}
}

/* Chon BIS: 1 BIS stereo, hoac cap FL + FR, hoac 1 BIS mono (ra ca hai loa), trong cac BIS
 * dien thoai yeu cau (tu tim: moi BIS); nhieu subgroup -> chon tan so cao nhat.
 */
static uint32_t select_bis(void)
{
	uint32_t best = 0;
	uint32_t best_hz = 0;

	for (uint8_t g = 0; g < base.n_sg; g++) {
		const struct bc_sg_info *sg = &base.sg[g];
		const uint32_t req = !assisted ? BT_BAP_BIS_SYNC_NO_PREF
					       : (g < want_nsg ? want_bis[g] : 0U);
		uint32_t stereo = 0, left = 0, right = 0, mono = 0, hz = 0, pick;

		if (req == 0U) {
			continue;
		}
		for (uint8_t i = 0; i < sg->n_bis; i++) {
			const struct bc_bis_info *b = &sg->bis[i];
			const uint32_t bit = BT_ISO_BIS_INDEX_BIT(b->index);
			const uint32_t a = b->cfg.chan_alloc;

			if (!b->ok || (req & bit) == 0U) {
				continue;
			}
			hz = b->cfg.freq_hz;
			if (b->cfg.chan_cnt == 2U) {
				stereo = stereo ? stereo : bit;
			} else if ((a & BT_AUDIO_LOCATION_FRONT_LEFT) &&
				   !(a & BT_AUDIO_LOCATION_FRONT_RIGHT)) {
				left = left ? left : bit;
			} else if ((a & BT_AUDIO_LOCATION_FRONT_RIGHT) &&
				   !(a & BT_AUDIO_LOCATION_FRONT_LEFT)) {
				right = right ? right : bit;
			} else {
				mono = mono ? mono : bit;
			}
		}
		pick = stereo ? stereo : (left && right) ? (left | right) : mono ? mono : (left | right);
		if (pick != 0U && hz > best_hz) {
			best = pick;
			best_hz = hz;
		}
	}
	return best;
}

static bool want_any(void)
{
	if (!assisted) {
		return true;
	}
	for (uint8_t i = 0; i < want_nsg; i++) {
		if (want_bis[i] != 0U) {
			return true;
		}
	}
	return false;
}

static void stop_big(void)
{
	int err = bt_bap_broadcast_sink_stop(sink);

	if (err && err != -EALREADY) {
		LOG_WRN("BIG stop failed (%d)", err);
	}
	big_bits = 0;
	set_mode(BC_PA_SYNCED);
}

/* Thong so BIG mot lan moi phien: khoang ISO / BN cho biet SDU den deu hay don cuc */
static void log_big_params(void)
{
	struct bc_big_params b;
	k_spinlock_key_t key;

	if (big_logged) {
		return;
	}
	big_logged = true;
	key = k_spin_lock(&lock);
	b = big_new;
	k_spin_unlock(&lock, key);

	LOG_INF("BIGInfo: %s, %u BIS, ISO interval %u us, SDU interval %u us, NSE %u, BN %u, "
		"IRC %u, PTO %u, max PDU %u B, max SDU %u B, PHY %u, %s",
		encrypted ? "encrypted" : "not encrypted", b.num_bis, b.iso_interval * 1250U,
		b.sdu_interval, b.nse, b.bn, b.irc, b.pto, b.max_pdu, b.max_sdu, b.phy,
		b.framing ? "framed" : "unframed");
}

static void log_base(void)
{
	LOG_INF("BASE: %u subgroup(s), presentation delay %d us", base.n_sg, base.pd_us);
	for (uint8_t g = 0; g < base.n_sg; g++) {
		for (uint8_t i = 0; i < base.sg[g].n_bis; i++) {
			const struct bc_bis_info *b = &base.sg[g].bis[i];

			if (b->ok) {
				LOG_INF("  subgroup %u BIS %u: LC3 %u Hz, %u us, %u ch (loc 0x%x), "
					"%u B",
					g, b->index, b->cfg.freq_hz, b->cfg.frame_dur_us,
					b->cfg.chan_cnt, b->cfg.chan_alloc, b->cfg.octets_per_frame);
			} else {
				LOG_INF("  subgroup %u BIS %u: not playable", g, b->index);
			}
		}
	}
}

/* Dong bo / dung BIG theo trang thai hien tai (goi cuoi moi lan xu ly) */
static void reconcile(void)
{
	uint32_t bits;
	int err;

	if (sink == NULL) {
		return;
	}

	if (mode == BC_BIG_SYNCING || mode == BC_STREAMING) {
		if (!want_any()) {
			LOG_INF("Phone asked to stop BIS sync");
			stop_big();
		} else if (bis_req_changed && select_bis() != big_bits) {
			LOG_INF("Phone changed BIS selection - resyncing");
			stop_big();
		}
	}
	bis_req_changed = false;

	if (mode != BC_PA_SYNCED || retry_wait || pa == NULL || !base_ok || !syncable ||
	    !want_any()) {
		return;
	}
	if (encrypted && !code_ok) {
		if (!warned) {
			warned = true;
			LOG_WRN("Broadcast is encrypted - %s",
				assisted ? "waiting for the code from the phone"
					 : "set the code with: helmet bc code <text> (used immediately)");
			if (!assisted) {
				audio_out_prompt(PROMPT_BC_FAIL); /* bao nguoi dung: can ma */
				search_prompt = false;
			}
		}
		return;
	}

	bits = select_bis();
	if (bits == 0U) {
		if (!warned) {
			warned = true;
			LOG_WRN("No playable BIS among those requested");
		}
		return;
	}

	err = bt_bap_broadcast_sink_sync(sink, bits, bcs_p, encrypted ? code : NULL);
	if (err) {
		LOG_ERR("BIG sync request failed (%d)", err);
		if (++retries > BC_MAX_RETRY) {
			give_up();
		} else {
			retry_wait = true;
			k_timer_start(&t_retry, K_MSEC(500), K_NO_WAIT);
		}
		return;
	}
	big_bits = bits;
	warned = false;
	set_mode(BC_BIG_SYNCING);
	LOG_INF("Syncing to BIG: BIS 0x%08x%s", bits, encrypted ? " (encrypted)" : "");
}

void bcast_handle(void)
{
	const atomic_val_t ev = atomic_clear(&ev_bits);
	k_spinlock_key_t key;

	if (ev == 0) {
		return;
	}

	/* 1. Ket thuc phien (lenh, dien thoai) */
	if (ev & BIT(EV_CMD_STOP)) {
		if (mode != BC_OFF) {
			end_session(PROMPT_BC_LEFT, true);
		}
	}
	if (ev & (BIT(EV_PA_TERM) | BIT(EV_SRC_REMOVED))) {
		on_session_end_req(ev);
	}

	/* 2. Thong tin broadcast (chi khi sink con ton tai: bo su kien cua phien cu) */
	if (sink != NULL && (ev & (BIT(EV_BASE) | BIT(EV_SYNCABLE)))) {
		key = k_spin_lock(&lock);
		if (ev & BIT(EV_BASE)) {
			base = base_new;
			base_ok = true;
		}
		if (ev & BIT(EV_SYNCABLE)) {
			syncable = true;
			encrypted = enc_new;
		}
		k_spin_unlock(&lock, key);
		if (ev & BIT(EV_BASE)) {
			log_base();
			if (search_prompt) {
				/* Da tim thay broadcast hop le: thoi am "dang tim" lap */
				audio_out_prompt_stop();
				search_prompt = false;
			}
		}
		if (ev & BIT(EV_SYNCABLE)) {
			log_big_params();
		}
	}

	/* 3. Ket qua tu controller */
	if (ev & BIT(EV_PA_LOST)) {
		on_pa_lost();
	}
	if (ev & BIT(EV_BIG_STOPPED)) {
		on_big_stopped();
	}
	if (ev & BIT(EV_BIG_STARTED)) {
		on_big_started();
	}

	/* 4. Yeu cau moi tu dien thoai, roi du lieu kem theo (ma, BIS). Ma tu dien thoai dung
	 * duoc ca cho nguon tu tim (thu vien bao "can ma" trong Receive State cuc bo).
	 */
	if (ev & BIT(EV_PA_REQ)) {
		on_pa_req();
	}
	if (mode != BC_OFF && (ev & (BIT(EV_CODE) | BIT(EV_BIS_REQ)))) {
		key = k_spin_lock(&lock);
		if (ev & BIT(EV_CODE)) {
			memcpy(code, req_code, sizeof(code));
			code_ok = true;
			warned = false;
		}
		if (assisted && (ev & BIT(EV_BIS_REQ))) {
			want_nsg = req_nsg;
			memcpy(want_bis, req_bis, sizeof(want_bis));
			bis_req_changed = true;
			warned = false;
		}
		k_spin_unlock(&lock, key);
	}

	if (ev & BIT(EV_USER_CODE)) {
		bool applied = false;

		key = k_spin_lock(&lock);
		if (user_code_set && mode != BC_OFF && (!assisted || !code_ok)) {
			memcpy(code, user_code, sizeof(code));
			code_ok = true;
			warned = false;
			applied = true;
		}
		k_spin_unlock(&lock, key);
		if (applied) {
			LOG_INF("Using the new broadcast code");
		}
	}

	/* 5. Tu tim, dong bo PA, hen gio */
	if (ev & BIT(EV_CMD_SCAN)) {
		if (mode != BC_OFF) {
			end_session(PROMPT_BC_LEFT, false);
		}
		start_scan();
	}
	if (ev & BIT(EV_HUB_FOUND)) {
		on_hub_found();
	}
	if (ev & BIT(EV_HUB_RESULT)) {
		on_hub_result();
	}
	if (ev & BIT(EV_T_CHOOSE)) {
		on_scan_choose();
	}
	if (ev & BIT(EV_REJOIN)) {
		on_rejoin();
	}
	if (ev & BIT(EV_PA_SYNCED)) {
		on_pa_synced();
	}
	if (ev & BIT(EV_T_PHASE)) {
		on_phase_timeout();
	}
	if (ev & BIT(EV_T_RETRY)) {
		retry_wait = false;
	}

	reconcile();
}

/* ---------------- Ma broadcast luu trong settings ("bcast/code") ---------------- */

static int bc_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t buf[BT_ISO_BROADCAST_CODE_SIZE] = {0};
	k_spinlock_key_t key;
	const char *next;

	if (settings_name_steq(name, "hub", &next) && next == NULL) {
		if (len != sizeof(saved) || read_cb(cb_arg, &saved, sizeof(saved)) != sizeof(saved) ||
		    saved.code_len > BT_ISO_BROADCAST_CODE_SIZE) {
			return -EINVAL;
		}
		saved_valid = true;
		return 0;
	}
	if (!settings_name_steq(name, "code", &next) || next != NULL) {
		return -ENOENT;
	}
	if (len == 0 || len > sizeof(buf) || read_cb(cb_arg, buf, len) != (ssize_t)len) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	memcpy(user_code, buf, sizeof(user_code));
	user_code_set = true;
	k_spin_unlock(&lock, key);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(bcast, "bcast", NULL, bc_settings_set, NULL, NULL);

/* ---------------- API ---------------- */

void bcast_toggle(void)
{
	k_spinlock_key_t key;

	if (mode != BC_OFF) {
		LOG_INF("Auracast: leaving broadcast (button)");
		rejoin_enabled = false; /* nguoi dung roi: thoi tu vao lai toi lan ghep / bat sau */
		k_timer_stop(&t_rejoin);
		end_session(PROMPT_BC_LEFT, true);
		return;
	}
	key = k_spin_lock(&lock);
	name_filter[0] = '\0'; /* nut: broadcast manh nhat, khong loc ten */
	k_spin_unlock(&lock, key);
	hub_retried = false;
	start_scan();
}

void bcast_leave(void)
{
	if (mode != BC_OFF) {
		LOG_INF("Auracast: leaving broadcast");
		end_session(PROMPT_BC_LEFT, false);
	}
}

/* Truoc bt_enable(): BASS la dich vu GATT dong (xem chu thich thu tu dang ky trong app/app_ctrl.c) */
int bcast_register(void)
{
	int err = bt_bap_scan_delegator_register(&sd_cbs);

	if (err) {
		LOG_ERR("Scan delegator (BASS) register failed (%d)", err);
	}
	return err;
}

int bcast_init(void)
{
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(bcs); i++) {
		bcs_p[i] = &bcs[i].stream;
		bt_bap_stream_cb_register(&bcs[i].stream, &bc_stream_ops);
	}

	err = bt_bap_broadcast_sink_register_cb(&sink_cbs);
	if (err) {
		LOG_ERR("Broadcast sink callbacks failed (%d)", err);
		return err;
	}
	(void)bt_le_per_adv_sync_cb_register(&pa_cbs);
	(void)bt_le_scan_cb_register(&scan_cbs);

	LOG_INF("Auracast sink: BASS (%u receive states), up to %u BIS, PAST %s",
		CONFIG_BT_BAP_SCAN_DELEGATOR_RECV_STATE_COUNT, BC_STREAMS,
		IS_ENABLED(CONFIG_BT_PER_ADV_SYNC_TRANSFER_RECEIVER) ? "yes" : "no");
	if (saved_valid) {
		/* settings_load() da chay: co hub da ghep -> tu vao lai sau vai giay */
		rejoin_enabled = true;
		k_timer_start(&t_rejoin, K_SECONDS(BC_REJOIN_FIRST_S), K_NO_WAIT);
		LOG_INF("Paired bike hub: broadcast 0x%06x - auto-join in %d s", saved.broadcast_id,
			BC_REJOIN_FIRST_S);
	}
	return 0;
}

/* ---------------- Shell / log ---------------- */

void bcast_print(const struct shell *sh)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (saved_valid) {
		shell_print(sh, "Bike hub  : paired, broadcast 0x%06x, SID %u, %s, auto-join %s",
			    saved.broadcast_id, saved.sid, saved.code_len ? "encrypted" : "open",
			    rejoin_enabled ? "on" : "off (left by button)");
	}
	if (mode == BC_OFF) {
		shell_print(sh, "Auracast  : off");
		return;
	}
	bt_addr_le_to_str(&tgt.addr, addr, sizeof(addr));
	shell_print(sh, "Auracast  : %s, %s, broadcast 0x%06x \"%s\" %s SID %u%s",
		    bcast_mode_str(mode), assisted ? "phone-assisted" : "self-scan",
		    tgt.broadcast_id, tgt.name, addr, tgt.sid, tgt.src_valid ? "" : " (no src)");
	shell_print(sh, "            BASE %s, BIGInfo %s%s, code %s, BIS req 0x%08x/0x%08x, "
		    "synced 0x%08x, retries %u",
		    base_ok ? "ok" : "-", syncable ? "ok" : "-",
		    syncable ? (encrypted ? " (encrypted)" : " (open)") : "",
		    code_ok ? "yes" : (user_code_set ? "no (saved code not used yet)" : "no"),
		    want_nsg > 0 ? want_bis[0] : 0U,
		    want_nsg > 1 ? want_bis[1] : 0U, big_bits, retries);
	for (size_t i = 0; i < ARRAY_SIZE(bcs); i++) {
		const struct bc_stream *s = &bcs[i];

		if (s->started) {
			shell_print(sh, "            stream %u: LC3 %u Hz %u ch loc 0x%x, rx %u, bad %u, %s",
				    (unsigned int)i, s->cfg.freq_hz, s->cfg.chan_cnt, s->cfg.chan_alloc, s->rx,
				    s->bad, s->out ? "playing" : "held (unicast)");
		}
	}
}

void bcast_log_status(void)
{
	if (mode == BC_OFF) {
		return;
	}
	LOG_INF("auracast: %s 0x%06x \"%s\", BIS 0x%08x, rx %u/%u, bad %u/%u%s",
		bcast_mode_str(mode), tgt.broadcast_id, tgt.name, big_bits, bcs[0].rx, bcs[1].rx,
		bcs[0].bad, bcs[1].bad, held ? ", held by unicast" : "");
}

int bcast_shell(const struct shell *sh, size_t argc, char **argv)
{
	k_spinlock_key_t key;

	if (argc < 2 || strcmp(argv[1], "status") == 0) {
		bcast_print(sh);
		return 0;
	}
	if (strcmp(argv[1], "scan") == 0) {
		key = k_spin_lock(&lock);
		if (argc > 2) {
			strncpy(name_filter, argv[2], sizeof(name_filter) - 1);
			name_filter[sizeof(name_filter) - 1] = '\0';
		} else {
			name_filter[0] = '\0';
		}
		k_spin_unlock(&lock, key);
		post(EV_CMD_SCAN);
		return 0;
	}
	if (strcmp(argv[1], "stop") == 0) {
		post(EV_CMD_STOP);
		return 0;
	}
	if (strcmp(argv[1], "forget") == 0) {
		saved_valid = false;
		rejoin_enabled = false;
		k_timer_stop(&t_rejoin);
		(void)settings_delete("bcast/hub");
		shell_print(sh, "Paired bike hub forgotten (no more auto-join)");
		return 0;
	}
	if (strcmp(argv[1], "code") == 0 && argc > 2) {
		const size_t n = strlen(argv[2]);
		int err;

		if (strcmp(argv[2], "clear") == 0) {
			key = k_spin_lock(&lock);
			user_code_set = false;
			memset(user_code, 0, sizeof(user_code));
			k_spin_unlock(&lock, key);
			err = settings_delete("bcast/code");
			shell_print(sh, "Broadcast code cleared%s", err ? " (delete from flash failed)" : "");
			return 0;
		}
		if (n == 0 || n > BT_ISO_BROADCAST_CODE_SIZE) {
			shell_error(sh, "Broadcast code is 1..%u characters", BT_ISO_BROADCAST_CODE_SIZE);
			return -EINVAL;
		}
		key = k_spin_lock(&lock);
		memset(user_code, 0, sizeof(user_code));
		memcpy(user_code, argv[2], n);
		user_code_set = true;
		k_spin_unlock(&lock, key);
		/* Luu nguyen chuoi (n byte); luc nap phan con lai cua 16 byte la 0 */
		err = settings_save_one("bcast/code", argv[2], n);
		post(EV_USER_CODE);
		shell_print(sh, "Broadcast code set (%u characters)%s - used for self-scan joins",
			    (unsigned int)n, err ? ", NOT saved (flash error)" : ", saved");
		return 0;
	}

	shell_error(sh, "usage: helmet bc [status|scan [name]|stop|forget|code <text>|code clear]");
	return -EINVAL;
}
