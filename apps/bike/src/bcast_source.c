/*
 * Bike hub - phia mu: BAP Broadcast Source (Auracast), 1 subgroup.
 *
 * Khung LC3 tu dien thoai (phone_sink.c) duoc chuyen NGUYEN VEN sang BIS, khong giai ma / ma hoa
 * lai: BIG dung dung cau hinh dien thoai chon (48 kHz, 10 ms, cung so octet). Dien thoai doi so
 * octet -> tao lai BIG (mu tu dong bo lai sau vai giay).
 *
 * Hai kieu BIG ("bike isoint 10|20", luu "bike/isoint"):
 *   20 ms (mac dinh): 1 BIS STEREO (SDU = [khung trai][khung phai]), chu ky ISO 20 ms, BN 2 (2 SDU
 *     10 ms moi su kien), tao bang LE Create BIG Test (CONFIG_BT_ISO_TEST_PARAMS). Ly do (do
 *     23/10/2026): CIS tu dien thoai va BIG cung chu ky 10 ms nhung khac dong ho -> pha troi cham;
 *     khi hai su kien chong nhau SDC luon uu tien BIG (muc 2) hon CIS (muc 3), ma su kien CIS la
 *     mot khoi -> CIS mat MOI su kien, chet 0x08 sau ~2 phut, dien thoai dung phat. BIG 20 ms chi
 *     chan toi da 1/2 su kien CIS; dien thoai dung FT >= 2 (hub de nghi tre 20 ms, phone_sink.c)
 *     gui lai goi bi chan o su kien CIS ke tiep. Can 2 x octet <= 251 B (48_4: 240 B).
 *   10 ms: 2 BIS (trai / phai), Create BIG thuong - kieu cu, de so sanh / du phong.
 * Controller khong nhan BIG 20 ms (tao that bai BN2_FAIL_MAX lan) -> tu dung 10 ms.
 *
 * Duong du lieu (moi 10 ms):
 *   bt_rx: phone_sink -> bcast_source_push_pair(L, R) -> hang doi cap khung (toi da Q_MAX)
 *   tx_work (system workqueue): lay cap khung, gui moi BIS (BIS stereo: ghep L+R thanh mot SDU),
 *     moi BIS toi da lay.inflight SDU nam trong controller; callback "sent" giai phong -> gui tiep.
 *   Dien thoai khong phat: hen gio 10 ms day khung "im lang" (ma hoa san) de mu van dong bo BIG
 *     va phat im lang sach.
 * Bu troi clock dien thoai / hub: hang doi day qua Q_MAX -> bo cap cu nhat; can -> controller
 * phat PDU rong, mu dung PLC (vai phut mot lan voi lech ~50 ppm).
 * Hai ben trai / phai luon di theo cap nen khong bao gio lech nhau.
 *
 * Tao lai BIG (doi octet / ma / RTN / chu ky, hoac controller tu ket thuc BIG) chay tren
 * workqueue RIENG (stack 4 KB) va bat dong bo: goi stop roi tra ve, callback "stopped" moi xoa +
 * tao lai. Truoc 23/10/2026 ham nay chay tren system workqueue va CHO "stopped" toi 5 s, trong khi
 * phan don dep BIS cung can system workqueue -> tu chan ~8 s, roi lc3_encode (bo dem MDCT
 * ~1,9 KB) lam tran stack 2560 B.
 *
 * Ma broadcast: mac dinh 16 byte NGAU NHIEN sinh lan dau, luu "bike/code"; khong ai phai go,
 * mu nhan qua Bike Hub Service (bhs.c) khi ghep bang nut. Bo phat broadcast dung dia chi
 * identity (co dinh) de mu da ghep tu dong bo lai moi lan bat ma khong can quet.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lc3.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "bike.h"

LOG_MODULE_REGISTER(bcast_src, LOG_LEVEL_INF);

#define NBIS_MAX   2
#define QLEN       8
#define Q_MAX      4 /* cap khung cho trong hub: vuot -> bo cap cu nhat (gioi han do tre) */
#define REPEAT_MAX 1 /* goi tu dien thoai mat: lap lai khung truoc 1 lan, sau do im lang */
/* RTN mac dinh 0 (23/10/2026). BIS 120 B ma hoa 2M ~0,69 ms / luot: BIG 10 ms 2 BIS RTN 1 =
 * 2,8 ms, RTN 2 = 4,1 ms; BIG 20 ms stereo 240 B ~1,17 ms / luot: RTN 0 = 2,3 ms, RTN 1 = 4,7 ms.
 * BIG cang dai cang chan nhieu su kien CIS cua dien thoai. Mu o gan xe (~1 m) nen mat goi it.
 */
#define RTN_DEFAULT 0
#define RTN_MAX     4
#define RTN_MAX_BN2 1 /* BIG 20 ms: BIG (<= 4,7 ms) + CIS (~4 ms) < 10 ms -> chi chan 1/2 su kien */
#define ISOINT_DEFAULT 20
#define BN2_FAIL_MAX   2  /* BIG 20 ms tao that bai lien tiep -> dung 10 ms */
/* SDU lon nhat: BIS stereo 2 x 125 B (1 PDU <= 251 B); BIS trai / phai <= BIKE_OCTETS_MAX */
#define TX_SDU_MAX     250
#define TX_BUFS        6 /* 2 BIS x (2 + 1) hoac 1 BIS x (3 + 1) */
#define BCAST_NAME     "Bike Audio"

#define BCAST_WQ_STACK 4096 /* lc3_encode (khung im lang) + goi BT */
#define BCAST_WQ_PRIO  K_PRIO_PREEMPT(5)

BUILD_ASSERT(CONFIG_BT_ISO_TX_MTU >= TX_SDU_MAX, "BIS stereo needs CONFIG_BT_ISO_TX_MTU >= 250");
BUILD_ASSERT(TX_SDU_MAX >= BIKE_OCTETS_MAX);

/* Kieu BIG dang chay (dat truoc khi tao BIG, chi doi khi running = 0) */
struct big_layout {
	uint8_t nbis;     /* 1: BIS stereo; 2: BIS trai + BIS phai */
	uint8_t bn;       /* SDU 10 ms moi BIS moi su kien BIG */
	uint8_t irc;      /* so luot phat moi PDU (= RTN + 1) */
	uint8_t iso_ms;   /* chu ky su kien BIG */
	uint8_t inflight; /* SDU toi da moi BIS nam trong controller */
};

struct bis_tx {
	struct bt_bap_stream stream;
	/* Trang thai ben trai (0) / phai (1) - dung ca khi BIS stereo */
	uint8_t last[BIKE_OCTETS_MAX]; /* khung hop le gan nhat (lap lai khi mat goi) */
	bool have_last;
	uint8_t repeat_run;
	uint32_t repeated;
	uint32_t silenced;
	/* Trang thai BIS thu i */
	uint16_t seq;
	bool started;
	atomic_t inflight;
	uint32_t sent;
	uint32_t send_err;
};

static struct bis_tx bis[NBIS_MAX];
static struct big_layout lay = {.nbis = 2, .bn = 1, .irc = 1, .iso_ms = 10, .inflight = 2};

/* Hang doi cap khung (giu lock) */
static struct k_spinlock lock;
static uint8_t q[QLEN][NBIS_MAX][BIKE_OCTETS_MAX];
static uint16_t q_len[QLEN];
static uint8_t q_rd;
static uint8_t q_wr;
static uint8_t q_cnt;
static uint32_t pushed;
static uint32_t dropped;
static uint32_t len_mismatch;
static uint32_t nobuf;
static uint32_t dry; /* controller het SDU trong luc dien thoai dang phat */

static uint8_t silence[BIKE_OCTETS_MAX];
static uint16_t silence_octets;               /* so octet cua khung im lang dang co */
static uint16_t octets = BIKE_OCTETS_DEFAULT; /* cua BIG dang chay */
static atomic_t running;                      /* moi BIS da started */
static atomic_t phone_active;
static atomic_t want_octets;

/* Cai dat luu ("bike/...") */
static uint32_t broadcast_id;
static bool bid_valid;
static uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
static uint8_t code_len; /* 0 = khong ma hoa */
static bool code_open;   /* nguoi dung chon khong ma hoa ("bike code open") */
static uint8_t rtn = RTN_DEFAULT;
static uint8_t isoint = ISOINT_DEFAULT;

static struct bt_le_ext_adv *adv;
static bool adv_running;
static struct bt_bap_broadcast_source *source;

/* Tao lai BIG (workqueue rieng) */
static K_THREAD_STACK_DEFINE(bcast_wq_stack, BCAST_WQ_STACK);
static struct k_work_q bcast_wq;
static bool wq_ready;
static atomic_t stopping;  /* da goi stop, cho callback "stopped" */
static atomic_t stop_done; /* callback "stopped" da den */
static atomic_t big_up;    /* BIG dang tao da started it nhat mot lan */
static atomic_t bn2_fail;  /* so lan BIG 20 ms tao that bai lien tiep */
static int64_t stop_t0;
static uint8_t delete_tries;

/* Preset BAP 48_4_1 (48 kHz, 10 ms, 120 B, RTN 4, 20 ms, PD 40 ms); octet, RTN va tham so test
 * chinh luc tao BIG. Phai la static: broadcast source giu con tro toi qos.
 */
static struct bt_bap_lc3_preset preset = BT_BAP_LC3_BROADCAST_PRESET_48_4_1(
	BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT, BT_AUDIO_CONTEXT_TYPE_MEDIA);

NET_BUF_POOL_FIXED_DEFINE(tx_pool, TX_BUFS, BT_ISO_SDU_BUF_SIZE(TX_SDU_MAX),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static void tx_work_fn(struct k_work *w);
static void restart_work_fn(struct k_work *w);
static K_WORK_DEFINE(tx_work, tx_work_fn);
static K_WORK_DELAYABLE_DEFINE(restart_work, restart_work_fn);

/* ---------------- Cai dat ---------------- */

static int bike_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "bid", &next) && !next) {
		if (len != sizeof(broadcast_id) ||
		    read_cb(cb_arg, &broadcast_id, sizeof(broadcast_id)) != sizeof(broadcast_id)) {
			return -EINVAL;
		}
		broadcast_id &= BT_AUDIO_BROADCAST_ID_MAX;
		bid_valid = true;
		return 0;
	}
	if (settings_name_steq(name, "code", &next) && !next) {
		if (len == 0 || len > BT_ISO_BROADCAST_CODE_SIZE) {
			return -EINVAL;
		}
		memset(code, 0, sizeof(code));
		if (read_cb(cb_arg, code, len) != (ssize_t)len) {
			return -EINVAL;
		}
		code_len = (uint8_t)len;
		return 0;
	}
	if (settings_name_steq(name, "open", &next) && !next) {
		uint8_t v = 0;

		if (len != 1 || read_cb(cb_arg, &v, 1) != 1) {
			return -EINVAL;
		}
		code_open = v != 0;
		return 0;
	}
	if (settings_name_steq(name, "rtn", &next) && !next) {
		if (len != 1 || read_cb(cb_arg, &rtn, 1) != 1 || rtn > RTN_MAX) {
			rtn = RTN_DEFAULT;
			return -EINVAL;
		}
		return 0;
	}
	if (settings_name_steq(name, "isoint", &next) && !next) {
		if (len != 1 || read_cb(cb_arg, &isoint, 1) != 1 || (isoint != 10 && isoint != 20)) {
			isoint = ISOINT_DEFAULT;
			return -EINVAL;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(bike, "bike", NULL, bike_settings_set, NULL, NULL);

/* ---------------- Khung im lang ---------------- */

/* Ma hoa 10 ms toan so 0 mot lan: mu nhan khung LC3 hop le, giai ma ra im lang sach.
 * lc3_encode can ~2,5 KB stack: chi goi tu main (khoi dong) hoac bcast_wq.
 */
static int make_silence(uint16_t oct)
{
	static lc3_encoder_mem_48k_t mem;
	static int16_t pcm[BIKE_FREQ_HZ / 100];
	lc3_encoder_t enc = lc3_setup_encoder(BIKE_FRAME_US, BIKE_FREQ_HZ, 0, &mem);

	if (enc == NULL) {
		return -EIO;
	}
	memset(pcm, 0, sizeof(pcm));
	if (lc3_encode(enc, LC3_PCM_FORMAT_S16, pcm, 1, oct, silence) != 0) {
		return -EIO;
	}
	silence_octets = oct;
	return 0;
}

/* ---------------- Kieu BIG ---------------- */

static void choose_layout(void)
{
	const bool stereo_fits = 2U * octets <= BT_ISO_PDU_MAX;

	if (IS_ENABLED(CONFIG_BT_ISO_TEST_PARAMS) && isoint == 20 && stereo_fits &&
	    atomic_get(&bn2_fail) < BN2_FAIL_MAX) {
		const uint8_t r = MIN(rtn, RTN_MAX_BN2);

		if (r != rtn) {
			LOG_WRN("RTN %u -> %u for the 20 ms BIG (longer BIG events block the phone CIS)",
				rtn, r);
		}
		lay = (struct big_layout){
			.nbis = 1, .bn = 2, .irc = r + 1, .iso_ms = 20, .inflight = 3};
		return;
	}
	if (isoint == 20) {
		LOG_WRN("20 ms BIG not possible (%s) - using 10 ms, 2 BIS",
			!IS_ENABLED(CONFIG_BT_ISO_TEST_PARAMS) ? "no CONFIG_BT_ISO_TEST_PARAMS"
			: !stereo_fits                         ? "stereo SDU > 251 B"
							       : "controller refused it");
	}
	lay = (struct big_layout){.nbis = 2, .bn = 1, .irc = rtn + 1, .iso_ms = 10, .inflight = 2};
}

static const char *layout_str(char *buf, size_t len)
{
	if (lay.nbis == 1) {
		snprintf(buf, len, "BIG %u ms, BN %u, 1 BIS stereo, RTN %u", lay.iso_ms, lay.bn,
			 lay.irc - 1U);
	} else {
		snprintf(buf, len, "BIG %u ms, 2 BIS L/R, RTN %u", lay.iso_ms, rtn);
	}
	return buf;
}

/* ---------------- Hang doi cap khung ---------------- */

static void queue_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	q_rd = 0;
	q_wr = 0;
	q_cnt = 0;
	for (size_t i = 0; i < NBIS_MAX; i++) {
		bis[i].have_last = false;
		bis[i].repeat_run = 0;
		atomic_set(&bis[i].inflight, 0);
	}
	k_spin_unlock(&lock, key);
}

/* Goi khi giu lock: chon khung cho ben i (mat goi -> lap lai 1 lan / im lang) */
static void fill_side(uint8_t i, uint8_t *dst, const uint8_t *src, uint16_t len)
{
	struct bis_tx *b = &bis[i];

	if (src != NULL) {
		memcpy(dst, src, len);
		memcpy(b->last, src, len);
		b->have_last = true;
		b->repeat_run = 0;
	} else if (b->have_last && b->repeat_run < REPEAT_MAX) {
		memcpy(dst, b->last, len);
		b->repeat_run++;
		b->repeated++;
	} else {
		memcpy(dst, silence, len);
		b->silenced++;
	}
}

/* Goi tu bt_rx (phone_sink) hoac tu ISR hen gio (khung im lang) */
void bcast_source_push_pair(const uint8_t *left, const uint8_t *right, uint16_t len)
{
	k_spinlock_key_t key;

	if (!atomic_get(&running)) {
		return;
	}

	key = k_spin_lock(&lock);
	if (len != octets) {
		len_mismatch++; /* BIG dang duoc tao lai voi so octet moi */
		k_spin_unlock(&lock, key);
		return;
	}
	if (q_cnt >= Q_MAX) {
		/* Dien thoai nhanh hon BIG (lech clock) hoac controller dang ket: bo cap cu nhat */
		q_rd = (q_rd + 1) % QLEN;
		q_cnt--;
		dropped++;
	}
	fill_side(0, q[q_wr][0], left, len);
	fill_side(1, q[q_wr][1], right, len);
	q_len[q_wr] = len;
	q_wr = (q_wr + 1) % QLEN;
	q_cnt++;
	pushed++;
	k_spin_unlock(&lock, key);

	k_work_submit(&tx_work);
}

/* System workqueue: thread duy nhat goi bt_bap_stream_send */
static void tx_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	while (atomic_get(&running)) {
		const uint8_t nbis = lay.nbis;
		struct net_buf *buf[NBIS_MAX] = {NULL};
		k_spinlock_key_t key;
		bool ok = true;

		/* Moi BIS phai con cho: gui theo cap de trai / phai luon cung su kien BIG */
		for (size_t i = 0; i < nbis; i++) {
			if (atomic_get(&bis[i].inflight) >= lay.inflight) {
				return;
			}
		}

		key = k_spin_lock(&lock);
		if (q_cnt == 0) {
			k_spin_unlock(&lock, key);
			return;
		}
		k_spin_unlock(&lock, key);

		for (size_t i = 0; i < nbis; i++) {
			buf[i] = net_buf_alloc(&tx_pool, K_NO_WAIT);
			if (buf[i] == NULL) {
				ok = false;
			} else {
				net_buf_reserve(buf[i], BT_ISO_CHAN_SEND_RESERVE);
			}
		}
		if (!ok) {
			for (size_t i = 0; i < nbis; i++) {
				if (buf[i] != NULL) {
					net_buf_unref(buf[i]);
				}
			}
			nobuf++;
			return;
		}

		key = k_spin_lock(&lock);
		if (nbis == 1) {
			/* BIS stereo: [trai][phai] theo thu tu bit vi tri (FL truoc FR) */
			net_buf_add_mem(buf[0], q[q_rd][0], q_len[q_rd]);
			net_buf_add_mem(buf[0], q[q_rd][1], q_len[q_rd]);
		} else {
			for (size_t i = 0; i < nbis; i++) {
				net_buf_add_mem(buf[i], q[q_rd][i], q_len[q_rd]);
			}
		}
		q_rd = (q_rd + 1) % QLEN;
		q_cnt--;
		k_spin_unlock(&lock, key);

		for (size_t i = 0; i < nbis; i++) {
			struct bis_tx *b = &bis[i];
			const int err = bt_bap_stream_send(&b->stream, buf[i], b->seq++);

			if (err) {
				net_buf_unref(buf[i]);
				b->send_err++;
			} else {
				atomic_inc(&b->inflight);
				b->sent++;
			}
		}
	}
}

/* Dien thoai khong phat: moi 10 ms mot cap khung im lang (cung dong ho GRTC voi BIG) */
static void idle_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	if (atomic_get(&running) && !atomic_get(&phone_active)) {
		bcast_source_push_pair(silence, silence, octets);
	}
}

static K_TIMER_DEFINE(idle_timer, idle_timer_fn, NULL);

/* ---------------- Callback luong BIS ---------------- */

static uint8_t bis_idx(const struct bt_bap_stream *stream)
{
	return (uint8_t)(CONTAINER_OF(stream, struct bis_tx, stream) - bis);
}

static void bis_started(struct bt_bap_stream *stream)
{
	struct bis_tx *b = CONTAINER_OF(stream, struct bis_tx, stream);
	struct bt_iso_info info;
	bool all = true;

	b->started = true;
	b->seq = 0;
	atomic_set(&b->inflight, 0);
	if (stream->iso != NULL && bt_iso_chan_get_info(stream->iso, &info) == 0) {
		LOG_INF("BIS %u started (BIG %u, BIS %u): ISO interval %u us, NSE %u, BN %u, IRC %u, "
			"PTO %u, PDU %u B, transport latency %u us",
			bis_idx(stream), info.broadcaster.big_handle, info.broadcaster.bis_number,
			info.iso_interval * 1250U, info.max_subevent, info.broadcaster.bn,
			info.broadcaster.irc, info.broadcaster.pto, info.broadcaster.max_pdu,
			info.broadcaster.latency);
	}
	for (size_t i = 0; i < lay.nbis; i++) {
		all &= bis[i].started;
	}
	if (all) {
		atomic_set(&running, 1);
		k_work_submit(&tx_work);
	}
}

static void bis_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	struct bis_tx *b = CONTAINER_OF(stream, struct bis_tx, stream);

	b->started = false;
	atomic_set(&running, 0);
	LOG_INF("BIS %u stopped (reason 0x%02x)", bis_idx(stream), reason);
}

static void bis_sent(struct bt_bap_stream *stream)
{
	struct bis_tx *b = CONTAINER_OF(stream, struct bis_tx, stream);

	/* SDU cua BIG cu (truoc khi tao lai) bao "sent" muon: bo qua, khong lam am bo dem */
	if (!b->started || atomic_get(&b->inflight) <= 0) {
		return;
	}
	if (atomic_dec(&b->inflight) <= 1 && atomic_get(&phone_active)) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		if (q_cnt == 0) {
			dry++; /* controller het SDU trong luc nhac dang phat */
		}
		k_spin_unlock(&lock, key);
	}
	k_work_submit(&tx_work);
}

static struct bt_bap_stream_ops bis_ops = {
	.started = bis_started,
	.stopped = bis_stopped,
	.sent = bis_sent,
};

static void source_started(struct bt_bap_broadcast_source *src)
{
	char l[64];

	ARG_UNUSED(src);
	atomic_set(&big_up, 1);
	atomic_set(&bn2_fail, 0);
	LOG_INF("Broadcast \"%s\" ID 0x%06x started: LC3 48 kHz, 10 ms, %u B, %s%s", BCAST_NAME,
		broadcast_id, octets, layout_str(l, sizeof(l)),
		code_len ? ", encrypted" : ", open");
}

/* Thread BT: BIG da dung (hub goi stop) hoac controller tu ket thuc / tao BIG that bai */
static void source_stopped(struct bt_bap_broadcast_source *src, uint8_t reason)
{
	ARG_UNUSED(src);
	atomic_set(&running, 0);
	LOG_INF("Broadcast stopped (reason 0x%02x)", reason);
	if (!wq_ready) {
		return;
	}
	if (atomic_get(&stopping)) {
		atomic_set(&stop_done, 1);
		k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_NO_WAIT);
		return;
	}
	if (!atomic_get(&big_up) && lay.bn > 1) {
		atomic_inc(&bn2_fail);
	}
	LOG_WRN("Broadcast ended without a stop request - rebuilding");
	k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_MSEC(200));
}

static struct bt_bap_broadcast_source_cb source_cbs = {
	.started = source_started,
	.stopped = source_stopped,
};

/* ---------------- Tao / bat dau BIG ---------------- */

static int adv_setup(void)
{
	NET_BUF_SIMPLE_DEFINE(ad_buf, BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	struct bt_data ad[2];
	int err;

	/* Nhu BT_BAP_ADV_PARAM_BROADCAST_FAST (60 ms) nhung dung dia chi identity: dia chi khong
	 * doi -> mu da ghep tao dong bo PA truc tiep theo dia chi + SID da luu.
	 */
	static const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_IDENTITY, BT_GAP_MS_TO_ADV_INTERVAL(60),
		BT_GAP_MS_TO_ADV_INTERVAL(60), NULL);

	err = bt_le_ext_adv_create(&param, NULL, &adv);
	if (err) {
		LOG_ERR("Broadcast advertising set create failed (%d)", err);
		return err;
	}
	err = bt_le_per_adv_set_param(adv, BT_BAP_PER_ADV_PARAM_BROADCAST_FAST);
	if (err) {
		LOG_ERR("Periodic advertising params failed (%d)", err);
		return err;
	}

	/* Broadcast Audio Announcement (UUID 0x1852 + Broadcast ID) + Broadcast Name */
	net_buf_simple_add_le16(&ad_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&ad_buf, broadcast_id);
	ad[0] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, ad_buf.data, ad_buf.len);
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_BROADCAST_NAME, BCAST_NAME,
					sizeof(BCAST_NAME) - 1);
	err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Broadcast advertising data failed (%d)", err);
	}
	return err;
}

/* Tao BIG theo "lay" (choose_layout() truoc) */
static int big_create_start(void)
{
	static const uint8_t left[] = {BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CFG_CHAN_ALLOC, BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_LEFT))};
	static const uint8_t right[] = {BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CFG_CHAN_ALLOC, BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_RIGHT))};
	static const uint8_t both[] = {BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
		BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT))};
	struct bt_bap_broadcast_source_stream_param sp[NBIS_MAX];
	struct bt_bap_broadcast_source_subgroup_param sg;
	struct bt_bap_broadcast_source_param p = {0};
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_data per_ad;
	int err;

	atomic_set(&big_up, 0);
	err = bt_audio_codec_cfg_set_octets_per_frame(&preset.codec_cfg, octets);
	if (err < 0) {
		LOG_ERR("Set octets failed (%d)", err);
		return err;
	}
	preset.qos.sdu = octets * (lay.nbis == 1 ? 2U : 1U);
	preset.qos.rtn = lay.irc - 1U;
#if defined(CONFIG_BT_ISO_TEST_PARAMS)
	if (lay.bn > 1) {
		/* LE Create BIG Test: chu ky ISO = bn x 10 ms, 1 SDU / PDU, khong pre-transmission */
		preset.qos.max_pdu = preset.qos.sdu;
		preset.qos.burst_number = lay.bn;
		preset.qos.num_subevents = lay.bn * lay.irc;
		p.irc = lay.irc;
		p.pto = 0;
		p.iso_interval = (uint16_t)(lay.iso_ms * 1000U / 1250U);
	} else {
		/* Tat ca = 0: Create BIG thuong, controller tu chon tham so */
		preset.qos.max_pdu = 0;
		preset.qos.burst_number = 0;
		preset.qos.num_subevents = 0;
	}
#endif /* CONFIG_BT_ISO_TEST_PARAMS */

	for (size_t i = 0; i < lay.nbis; i++) {
		const uint8_t *d = lay.nbis == 1 ? both : (i == 0 ? left : right);

		sp[i].stream = &bis[i].stream;
		sp[i].data = (uint8_t *)d;
		sp[i].data_len = sizeof(left);
	}
	sg.params_count = lay.nbis;
	sg.params = sp;
	sg.codec_cfg = &preset.codec_cfg;
	p.params_count = 1;
	p.params = &sg;
	p.qos = &preset.qos;
	p.packing = BT_ISO_PACKING_SEQUENTIAL;
	p.encryption = code_len > 0;
	if (p.encryption) {
		memcpy(p.broadcast_code, code, code_len); /* phan con lai = 0 */
	}

	err = bt_bap_broadcast_source_create(&p, &source);
	if (err) {
		LOG_ERR("Broadcast source create failed (%d)", err);
		source = NULL;
		return err;
	}

	/* BASE (codec, so octet, vi tri tung BIS) nam trong periodic advertising */
	err = bt_bap_broadcast_source_get_base(source, &base_buf);
	if (err) {
		LOG_ERR("Get BASE failed (%d)", err);
		return err;
	}
	per_ad = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, base_buf.data, base_buf.len);
	err = bt_le_per_adv_set_data(adv, &per_ad, 1);
	if (err) {
		LOG_ERR("Periodic advertising data failed (%d)", err);
		return err;
	}

	if (!adv_running) {
		err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
		if (err) {
			LOG_ERR("Broadcast advertising start failed (%d)", err);
			return err;
		}
		err = bt_le_per_adv_start(adv);
		if (err) {
			LOG_ERR("Periodic advertising start failed (%d)", err);
			return err;
		}
		adv_running = true;
	}

	err = bt_bap_broadcast_source_start(source, adv);
	if (err) {
		LOG_ERR("Broadcast source start failed (%d)", err);
	}
	return err;
}

/* bcast_wq: dung BIG (bat dong bo), xoa, tao lai voi cau hinh moi (octet, ma, RTN, chu ky).
 * Moi buoc khong bao gio ngu / cho: can doi thi hen chay lai chinh no. That bai thi tu thu lai
 * sau 1 s - broadcast khong bao gio DOWN luon.
 */
#define STOP_WAIT_MS     5000
#define DELETE_TRIES     20
#define DELETE_RETRY_MS  100
#define RESTART_RETRY_MS 1000

static void restart_work_fn(struct k_work *w)
{
	uint16_t want;
	char l[64];
	int err;

	ARG_UNUSED(w);

	atomic_set(&running, 0);
	if (source != NULL) {
		if (!atomic_get(&stopping)) {
			atomic_set(&stop_done, 0);
			atomic_set(&stopping, 1);
			stop_t0 = k_uptime_get();
			/* Het gio truoc, roi moi stop: callback "stopped" den som van gap duoc */
			k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_MSEC(STOP_WAIT_MS));
			err = bt_bap_broadcast_source_stop(source);
			if (err == 0) {
				return; /* tiep tuc khi source_stopped() goi lai */
			}
			/* BIG khong chay (-EBADMSG / -EALREADY): xoa luon */
			(void)k_work_cancel_delayable(&restart_work);
			atomic_set(&stop_done, 1);
		} else if (!atomic_get(&stop_done)) {
			const int64_t left_ms = STOP_WAIT_MS - (k_uptime_get() - stop_t0);

			if (left_ms > 0) {
				/* Yeu cau tao lai moi trong luc dang dung: cho tiep */
				k_work_reschedule_for_queue(&bcast_wq, &restart_work,
							    K_MSEC(left_ms));
				if (atomic_get(&stop_done)) {
					k_work_reschedule_for_queue(&bcast_wq, &restart_work,
								    K_NO_WAIT);
				}
				return;
			}
			LOG_WRN("Broadcast stop not confirmed after %d ms", STOP_WAIT_MS);
		}
		err = bt_bap_broadcast_source_delete(source);
		if (err == -EBADMSG && ++delete_tries < DELETE_TRIES) {
			/* BIS chua dung han */
			k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_MSEC(DELETE_RETRY_MS));
			return;
		}
		delete_tries = 0;
		atomic_set(&stopping, 0);
		if (err) {
			LOG_ERR("Broadcast source delete failed (%d) - retry in %d ms", err,
				RESTART_RETRY_MS);
			k_work_reschedule_for_queue(&bcast_wq, &restart_work,
						    K_MSEC(RESTART_RETRY_MS));
			return;
		}
		source = NULL;
	}
	for (size_t i = 0; i < NBIS_MAX; i++) {
		bis[i].started = false;
	}

	/* So octet dien thoai yeu cau (0 = giu nguyen); chi lay ra o buoc nay */
	want = (uint16_t)atomic_clear(&want_octets);
	if (want >= BIKE_OCTETS_MIN && want <= BIKE_OCTETS_MAX) {
		octets = want;
	}
	if (octets != silence_octets && make_silence(octets) != 0) {
		LOG_ERR("LC3 silence frame encode failed");
	}
	choose_layout();
	queue_reset();
	LOG_INF("Restarting broadcast: %u B/frame, %s, %s", octets, layout_str(l, sizeof(l)),
		code_len ? "encrypted" : "open");
	err = big_create_start();
	if (err) {
		if (lay.bn > 1) {
			atomic_inc(&bn2_fail);
		}
		LOG_ERR("Broadcast restart failed (%d) - retry in %d ms", err, RESTART_RETRY_MS);
		k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_MSEC(RESTART_RETRY_MS));
	}
}

static int request_restart(void)
{
	if (!wq_ready) {
		return -EAGAIN;
	}
	k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_NO_WAIT);
	return 0;
}

/* ---------------- API ---------------- */

void bcast_source_phone_started(uint16_t oct)
{
	atomic_set(&phone_active, 1);
	if (oct != octets && wq_ready) {
		LOG_INF("Phone uses %u B/frame (broadcast %u B) - rebuilding broadcast", oct, octets);
		atomic_set(&want_octets, oct);
		k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_NO_WAIT);
	}
}

void bcast_source_phone_stopped(void)
{
	atomic_set(&phone_active, 0);
}

static int new_random_code(void)
{
	int err = bt_rand(code, sizeof(code));

	if (err) {
		return err;
	}
	code_len = sizeof(code);
	return settings_save_one("bike/code", code, code_len);
}

int bcast_source_set_code(enum bike_code_mode mode, const char *text)
{
	const size_t n = text ? strlen(text) : 0;
	const uint8_t open = mode == BIKE_CODE_OPEN;
	int err = 0;

	if (mode == BIKE_CODE_TEXT && (n == 0 || n > BT_ISO_BROADCAST_CODE_SIZE)) {
		return -EINVAL;
	}
	memset(code, 0, sizeof(code));
	code_len = 0;
	if (mode == BIKE_CODE_RANDOM) {
		err = new_random_code();
	} else if (mode == BIKE_CODE_TEXT) {
		memcpy(code, text, n);
		code_len = (uint8_t)n;
		err = settings_save_one("bike/code", code, code_len);
	} else {
		(void)settings_delete("bike/code");
	}
	code_open = open;
	(void)settings_save_one("bike/open", &open, 1);
	(void)request_restart();
	return err;
}

void bcast_source_join_info(uint8_t *out)
{
	struct bt_le_ext_adv_info info;

	memset(out, 0, BHS_JOIN_INFO_LEN);
	out[0] = BHS_PROTO_VER;
	sys_put_le24(broadcast_id, &out[1]);
	if (adv != NULL && bt_le_ext_adv_get_info(adv, &info) == 0 && info.addr != NULL) {
		out[4] = info.addr->type;
		memcpy(&out[5], info.addr->a.val, sizeof(info.addr->a.val));
		out[11] = info.sid;
	}
	out[12] = code_len;
	memcpy(&out[13], code, code_len);
}

int bcast_source_set_rtn(uint8_t r)
{
	if (r > RTN_MAX) {
		return -EINVAL;
	}
	rtn = r;
	(void)request_restart();
	return settings_save_one("bike/rtn", &rtn, 1);
}

int bcast_source_set_isoint(uint8_t ms)
{
	if (ms != 10 && ms != 20) {
		return -EINVAL;
	}
	isoint = ms;
	atomic_set(&bn2_fail, 0); /* thu lai BIG 20 ms du lan truoc controller tu choi */
	(void)request_restart();
	return settings_save_one("bike/isoint", &isoint, 1);
}

uint8_t bcast_source_iso_ms(void)
{
	return lay.iso_ms;
}

int bcast_source_init(void)
{
	static const struct k_work_queue_config wq_cfg = {.name = "bcast_wq"};
	char l[64];
	int err;

	k_work_queue_init(&bcast_wq);
	k_work_queue_start(&bcast_wq, bcast_wq_stack, K_THREAD_STACK_SIZEOF(bcast_wq_stack),
			   BCAST_WQ_PRIO, &wq_cfg);
	wq_ready = true;

	if (!bid_valid) {
		/* Broadcast ID co dinh suot doi thiet bi: mu nho duoc nguon cua xe minh */
		err = bt_rand(&broadcast_id, BT_AUDIO_BROADCAST_ID_SIZE);
		if (err) {
			return err;
		}
		broadcast_id &= BT_AUDIO_BROADCAST_ID_MAX;
		(void)settings_save_one("bike/bid", &broadcast_id, sizeof(broadcast_id));
		bid_valid = true;
	}
	if (code_len == 0 && !code_open) {
		/* Lan dau: ma ngau nhien, chi mu ghep bang nut moi nhan duoc */
		err = new_random_code();
		if (err) {
			LOG_WRN("Random broadcast code failed (%d) - broadcast open", err);
			code_len = 0;
		}
	}

	for (size_t i = 0; i < NBIS_MAX; i++) {
		bt_bap_stream_cb_register(&bis[i].stream, &bis_ops);
	}
	err = bt_bap_broadcast_source_register_cb(&source_cbs);
	if (err) {
		LOG_ERR("Broadcast source callbacks failed (%d)", err);
		return err;
	}

	err = make_silence(octets); /* thread main, stack 3 KB */
	if (err) {
		LOG_ERR("LC3 silence frame encode failed (%d)", err);
		return err;
	}
	err = adv_setup();
	if (err) {
		return err;
	}
	choose_layout();
	LOG_INF("Broadcast layout: %s", layout_str(l, sizeof(l)));
	err = big_create_start();
	if (err) {
		/* vd controller tu choi BIG 20 ms: bcast_wq don dep va thu lai (sau 2 lan -> 10 ms) */
		if (lay.bn > 1) {
			atomic_inc(&bn2_fail);
		}
		k_work_reschedule_for_queue(&bcast_wq, &restart_work, K_MSEC(RESTART_RETRY_MS));
	}
	k_timer_start(&idle_timer, K_USEC(BIKE_FRAME_US), K_USEC(BIKE_FRAME_US));
	return 0;
}

void bcast_source_print(const struct shell *sh)
{
	char l[64];

	shell_print(sh, "Broadcast : \"%s\" ID 0x%06x, %s, LC3 48 kHz 10 ms %u B, %s, %s",
		    BCAST_NAME, broadcast_id, atomic_get(&running) ? "RUNNING" : "not running",
		    octets, layout_str(l, sizeof(l)),
		    code_len ? "encrypted (code given to helmets by button pairing)" : "open (no code)");
	shell_print(sh, "Settings  : isoint %u ms, rtn %u (20 ms refused %ld time(s))", isoint, rtn,
		    atomic_get(&bn2_fail));
	shell_print(sh, "Relay     : phone %s, queue %u/%u, pushed %u, dropped %u, dry %u, "
		    "len mismatch %u, no buf %u",
		    atomic_get(&phone_active) ? "streaming" : "idle (sending silence)", q_cnt, Q_MAX,
		    pushed, dropped, dry, len_mismatch, nobuf);
	for (size_t i = 0; i < NBIS_MAX; i++) {
		const struct bis_tx *b = &bis[i];

		shell_print(sh, "Side %c    : repeated %u, silence %u", i == 0 ? 'L' : 'R',
			    b->repeated, b->silenced);
	}
	for (size_t i = 0; i < lay.nbis; i++) {
		const struct bis_tx *b = &bis[i];

		shell_print(sh, "BIS %u     : %s, sent %u, in controller %d/%u, send err %u",
			    (unsigned)i, lay.nbis == 1 ? "L+R" : (i == 0 ? "L" : "R"), b->sent,
			    (int)atomic_get(&b->inflight), lay.inflight, b->send_err);
	}
}

/* Thread main (heartbeat 10 s): so lieu TRONG 10 s vua qua, de thay ngay luc nao bat thuong */
void bcast_source_log_status(void)
{
	static uint32_t l_pushed, l_sent, l_dropped, l_dry, l_nobuf, l_err, l_rep, l_sil;
	const uint32_t sent = bis[0].sent;
	const uint32_t err = bis[0].send_err + bis[1].send_err;
	const uint32_t rep = bis[0].repeated;
	const uint32_t sil = bis[0].silenced;
	char l[64];
	char ctl[16];

	if (lay.nbis > 1) {
		snprintf(ctl, sizeof(ctl), "%d/%d", (int)atomic_get(&bis[0].inflight),
			 (int)atomic_get(&bis[1].inflight));
	} else {
		snprintf(ctl, sizeof(ctl), "%d", (int)atomic_get(&bis[0].inflight));
	}
	LOG_INF("broadcast: %s, %s, phone %s | 10 s: pushed %u, sent %u, dropped %u, dry %u, "
		"repeat %u, silence %u, no buf %u, err %u | in ctrl %s, queue %u",
		atomic_get(&running) ? "running" : "DOWN", layout_str(l, sizeof(l)),
		atomic_get(&phone_active) ? "streaming" : "idle", pushed - l_pushed, sent - l_sent,
		dropped - l_dropped, dry - l_dry, rep - l_rep, sil - l_sil, nobuf - l_nobuf,
		err - l_err, ctl, q_cnt);
	l_pushed = pushed;
	l_sent = sent;
	l_dropped = dropped;
	l_dry = dry;
	l_nobuf = nobuf;
	l_err = err;
	l_rep = rep;
	l_sil = sil;
}
