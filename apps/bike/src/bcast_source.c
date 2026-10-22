/*
 * Bike hub - phia mu: BAP Broadcast Source (Auracast), 1 subgroup x 2 BIS (trai / phai).
 *
 * Khung LC3 tu dien thoai (phone_sink.c) duoc chuyen NGUYEN VEN sang BIS, khong giai ma / ma hoa
 * lai: BIG dung dung cau hinh dien thoai chon (48 kHz, 10 ms, cung so octet). Dien thoai doi so
 * octet -> tao lai BIG (mu tu dong bo lai sau vai giay).
 *
 * Duong du lieu (moi 10 ms):
 *   bt_rx: phone_sink -> bcast_source_push_pair(L, R) -> hang doi cap khung (toi da Q_MAX)
 *   tx_work (system workqueue): lay cap khung, gui ca hai BIS cung luc, moi BIS toi da
 *     INFLIGHT SDU nam trong controller; callback "sent" giai phong cho -> gui tiep.
 *   Dien thoai khong phat: hen gio 10 ms day khung "im lang" (ma hoa san luc khoi dong) de mu
 *     van dong bo BIG va phat im lang sach.
 * Bu troi clock dien thoai / hub: hang doi day qua Q_MAX -> bo cap cu nhat; can -> controller
 * phat PDU rong, mu dung PLC (vai phut mot lan voi lech ~50 ppm).
 * Hai BIS luon nhan SDU theo cap nen trai / phai khong bao gio lech nhau.
 *
 * Ma broadcast: mac dinh 16 byte NGAU NHIEN sinh lan dau, luu "bike/code"; khong ai phai go,
 * mu nhan qua Bike Hub Service (bhs.c) khi ghep bang nut. Bo phat broadcast dung dia chi
 * identity (co dinh) de mu da ghep tu dong bo lai moi lan bat ma khong can quet.
 */

#include <errno.h>
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

#define NBIS       2
#define QLEN       8
#define Q_MAX      4 /* cap khung cho trong hub: vuot -> bo cap cu nhat (gioi han do tre) */
#define INFLIGHT   2 /* SDU moi BIS dua vao controller cung luc */
#define REPEAT_MAX 1 /* goi tu dien thoai mat: lap lai khung truoc 1 lan, sau do im lang */
#define RTN_DEFAULT 2
#define RTN_MAX     4
#define BCAST_NAME  "Bike Audio"

struct bis_tx {
	struct bt_bap_stream stream;
	uint8_t last[BIKE_OCTETS_MAX]; /* khung hop le gan nhat (lap lai khi mat goi) */
	bool have_last;
	uint8_t repeat_run;
	uint16_t seq;
	bool started;
	atomic_t inflight;
	uint32_t sent;
	uint32_t send_err;
	uint32_t repeated;
	uint32_t silenced;
};

static struct bis_tx bis[NBIS];

/* Hang doi cap khung (giu lock) */
static struct k_spinlock lock;
static uint8_t q[QLEN][NBIS][BIKE_OCTETS_MAX];
static uint16_t q_len[QLEN];
static uint8_t q_rd;
static uint8_t q_wr;
static uint8_t q_cnt;
static uint32_t pushed;
static uint32_t dropped;
static uint32_t len_mismatch;
static uint32_t nobuf;
static uint32_t dry; /* controller can SDU trong luc dien thoai dang phat */

static uint8_t silence[BIKE_OCTETS_MAX];
static uint16_t octets = BIKE_OCTETS_DEFAULT; /* cua BIG dang chay */
static atomic_t running;                      /* ca hai BIS da started */
static atomic_t phone_active;
static atomic_t want_octets;
/* Che do Dinh vi (nav_hub.c): tam dung BIG de nhuong radio cho Channel Sounding. BIG 10 ms (ca khi
 * chi phat khung im lang) lam hau het thu tuc CS mu-xe bi huy (do 22/10/2026).
 */
static atomic_t paused;

/* Cai dat luu ("bike/...") */
static uint32_t broadcast_id;
static bool bid_valid;
static uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
static uint8_t code_len; /* 0 = khong ma hoa */
static bool code_open;   /* nguoi dung chon khong ma hoa ("bike code open") */
static uint8_t rtn = RTN_DEFAULT;

static struct bt_le_ext_adv *adv;
static bool adv_running;
static struct bt_bap_broadcast_source *source;
static K_SEM_DEFINE(sem_stopped, 0, 1);

/* Preset BAP 48_4_1 (48 kHz, 10 ms, 120 B, RTN 4, 20 ms, PD 40 ms); octet va RTN chinh luc
 * tao BIG. Phai la static: broadcast source giu con tro toi qos.
 */
static struct bt_bap_lc3_preset preset = BT_BAP_LC3_BROADCAST_PRESET_48_4_1(
	BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT, BT_AUDIO_CONTEXT_TYPE_MEDIA);

NET_BUF_POOL_FIXED_DEFINE(tx_pool, NBIS * (INFLIGHT + 1), BT_ISO_SDU_BUF_SIZE(BIKE_OCTETS_MAX),
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
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(bike, "bike", NULL, bike_settings_set, NULL, NULL);

/* ---------------- Khung im lang ---------------- */

/* Ma hoa 10 ms toan so 0 mot lan: mu nhan khung LC3 hop le, giai ma ra im lang sach */
static int make_silence(uint16_t oct)
{
	static lc3_encoder_mem_48k_t mem;
	static int16_t pcm[BIKE_FREQ_HZ / 100];
	lc3_encoder_t enc = lc3_setup_encoder(BIKE_FRAME_US, BIKE_FREQ_HZ, 0, &mem);

	if (enc == NULL) {
		return -EIO;
	}
	memset(pcm, 0, sizeof(pcm));
	return lc3_encode(enc, LC3_PCM_FORMAT_S16, pcm, 1, oct, silence) == 0 ? 0 : -EIO;
}

/* ---------------- Hang doi cap khung ---------------- */

static void queue_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	q_rd = 0;
	q_wr = 0;
	q_cnt = 0;
	for (size_t i = 0; i < NBIS; i++) {
		bis[i].have_last = false;
		bis[i].repeat_run = 0;
		atomic_set(&bis[i].inflight, 0);
	}
	k_spin_unlock(&lock, key);
}

/* Goi khi giu lock: chon khung cho BIS i (mat goi -> lap lai 1 lan / im lang) */
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
		struct net_buf *buf[NBIS];
		k_spinlock_key_t key;
		bool ok = true;

		/* Ca hai BIS phai con cho: gui theo cap de trai / phai luon cung su kien BIG */
		for (size_t i = 0; i < NBIS; i++) {
			if (atomic_get(&bis[i].inflight) >= INFLIGHT) {
				return;
			}
		}

		key = k_spin_lock(&lock);
		if (q_cnt == 0) {
			k_spin_unlock(&lock, key);
			return;
		}
		k_spin_unlock(&lock, key);

		for (size_t i = 0; i < NBIS; i++) {
			buf[i] = net_buf_alloc(&tx_pool, K_NO_WAIT);
			if (buf[i] == NULL) {
				ok = false;
			} else {
				net_buf_reserve(buf[i], BT_ISO_CHAN_SEND_RESERVE);
			}
		}
		if (!ok) {
			for (size_t i = 0; i < NBIS; i++) {
				if (buf[i] != NULL) {
					net_buf_unref(buf[i]);
				}
			}
			nobuf++;
			return;
		}

		key = k_spin_lock(&lock);
		for (size_t i = 0; i < NBIS; i++) {
			net_buf_add_mem(buf[i], q[q_rd][i], q_len[q_rd]);
		}
		q_rd = (q_rd + 1) % QLEN;
		q_cnt--;
		k_spin_unlock(&lock, key);

		for (size_t i = 0; i < NBIS; i++) {
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
		LOG_INF("BIS %u started (BIG %u, BIS number %u)", bis_idx(stream),
			info.broadcaster.big_handle, info.broadcaster.bis_number);
	}
	for (size_t i = 0; i < NBIS; i++) {
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
			dry++; /* controller sap het SDU trong luc nhac dang phat */
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
	ARG_UNUSED(src);
	LOG_INF("Broadcast \"%s\" ID 0x%06x started: LC3 48 kHz, 10 ms, %u B, RTN %u%s",
		BCAST_NAME, broadcast_id, octets, rtn, code_len ? ", encrypted" : ", open");
}

static void source_stopped(struct bt_bap_broadcast_source *src, uint8_t reason)
{
	ARG_UNUSED(src);
	LOG_INF("Broadcast stopped (reason 0x%02x)", reason);
	k_sem_give(&sem_stopped);
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

static int big_create_start(void)
{
	static const uint8_t left[] = {BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CFG_CHAN_ALLOC, BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_LEFT))};
	static const uint8_t right[] = {BT_AUDIO_CODEC_DATA(
		BT_AUDIO_CODEC_CFG_CHAN_ALLOC, BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_RIGHT))};
	struct bt_bap_broadcast_source_stream_param sp[NBIS];
	struct bt_bap_broadcast_source_subgroup_param sg;
	struct bt_bap_broadcast_source_param p = {0};
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_data per_ad;
	int err;

	err = bt_audio_codec_cfg_set_octets_per_frame(&preset.codec_cfg, octets);
	if (err < 0) {
		LOG_ERR("Set octets failed (%d)", err);
		return err;
	}
	preset.qos.sdu = octets;
	preset.qos.rtn = rtn;

	for (size_t i = 0; i < NBIS; i++) {
		sp[i].stream = &bis[i].stream;
		sp[i].data = (uint8_t *)(i == 0 ? left : right);
		sp[i].data_len = sizeof(left);
	}
	sg.params_count = NBIS;
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

/* System workqueue: dung BIG, tao lai voi cau hinh moi (so octet, ma, RTN).
 * Controller co the mat ~2 s moi bao BIG da dung (do 20/10/2026: 1.9 s): cho toi 5 s, xoa lai
 * neu con "sai trang thai", that bai thi tu thu lai sau 1 s - broadcast khong bao gio DOWN luon.
 */
#define STOP_WAIT_S      5
#define DELETE_TRIES     10
#define DELETE_RETRY_MS  200
#define RESTART_RETRY_MS 1000

static void restart_work_fn(struct k_work *w)
{
	const uint16_t want = (uint16_t)atomic_get(&want_octets);
	int err;

	ARG_UNUSED(w);

	atomic_set(&running, 0);
	if (source != NULL) {
		k_sem_reset(&sem_stopped);
		err = bt_bap_broadcast_source_stop(source);
		if (err == 0 && k_sem_take(&sem_stopped, K_SECONDS(STOP_WAIT_S)) != 0) {
			LOG_WRN("Broadcast stop not confirmed after %d s", STOP_WAIT_S);
		}
		for (int i = 0; i < DELETE_TRIES; i++) {
			err = bt_bap_broadcast_source_delete(source);
			if (err != -EBADMSG) {
				break;
			}
			k_msleep(DELETE_RETRY_MS); /* BIS chua dung han */
		}
		if (err) {
			LOG_ERR("Broadcast source delete failed (%d) - retry in %d ms", err,
				RESTART_RETRY_MS);
			k_work_reschedule(&restart_work, K_MSEC(RESTART_RETRY_MS));
			return;
		}
		source = NULL;
	}
	for (size_t i = 0; i < NBIS; i++) {
		bis[i].started = false;
	}
	if (atomic_get(&paused)) {
		queue_reset();
		LOG_INF("Broadcast paused (navigation mode): BIG stopped, advertising kept");
		return;
	}

	if (want >= BIKE_OCTETS_MIN && want <= BIKE_OCTETS_MAX) {
		octets = want;
	}
	if (make_silence(octets) != 0) {
		LOG_ERR("LC3 silence frame encode failed");
	}
	queue_reset();
	LOG_INF("Restarting broadcast: %u B/frame, RTN %u, %s", octets, rtn,
		code_len ? "encrypted" : "open");
	err = big_create_start();
	if (err) {
		LOG_ERR("Broadcast restart failed (%d) - retry in %d ms", err, RESTART_RETRY_MS);
		k_work_reschedule(&restart_work, K_MSEC(RESTART_RETRY_MS));
	}
}

/* ---------------- API ---------------- */

void bcast_source_phone_started(uint16_t oct)
{
	atomic_set(&phone_active, 1);
	if (oct != octets) {
		LOG_INF("Phone uses %u B/frame (broadcast %u B) - rebuilding broadcast", oct, octets);
		atomic_set(&want_octets, oct);
		k_work_reschedule(&restart_work, K_NO_WAIT);
	}
}

void bcast_source_phone_stopped(void)
{
	atomic_set(&phone_active, 0);
}

void bcast_source_pause(bool pause)
{
	if ((atomic_set(&paused, pause ? 1 : 0) != 0) == pause) {
		return;
	}
	LOG_INF("Broadcast %s", pause ? "pausing for navigation mode" : "resuming");
	atomic_set(&want_octets, octets);
	k_work_reschedule(&restart_work, K_NO_WAIT);
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
	atomic_set(&want_octets, octets);
	k_work_reschedule(&restart_work, K_NO_WAIT);
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
	atomic_set(&want_octets, octets);
	k_work_reschedule(&restart_work, K_NO_WAIT);
	return settings_save_one("bike/rtn", &rtn, 1);
}

int bcast_source_init(void)
{
	int err;

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

	for (size_t i = 0; i < NBIS; i++) {
		bt_bap_stream_cb_register(&bis[i].stream, &bis_ops);
	}
	err = bt_bap_broadcast_source_register_cb(&source_cbs);
	if (err) {
		LOG_ERR("Broadcast source callbacks failed (%d)", err);
		return err;
	}

	err = make_silence(octets);
	if (err) {
		LOG_ERR("LC3 silence frame encode failed (%d)", err);
		return err;
	}
	err = adv_setup();
	if (err) {
		return err;
	}
	err = big_create_start();
	if (err) {
		return err;
	}
	k_timer_start(&idle_timer, K_USEC(BIKE_FRAME_US), K_USEC(BIKE_FRAME_US));
	return 0;
}

void bcast_source_print(const struct shell *sh)
{
	shell_print(sh, "Broadcast : \"%s\" ID 0x%06x, %s, LC3 48 kHz 10 ms %u B, RTN %u, %s",
		    BCAST_NAME, broadcast_id,
		    atomic_get(&running) ? "RUNNING"
					 : (atomic_get(&paused) ? "paused (navigation)" : "not running"),
		    octets, rtn,
		    code_len ? "encrypted (code given to helmets by button pairing)" : "open (no code)");
	shell_print(sh, "Relay     : phone %s, queue %u/%u, pushed %u, dropped %u, dry %u, "
		    "len mismatch %u, no buf %u",
		    atomic_get(&phone_active) ? "streaming" : "idle (sending silence)", q_cnt, Q_MAX,
		    pushed, dropped, dry, len_mismatch, nobuf);
	for (size_t i = 0; i < NBIS; i++) {
		const struct bis_tx *b = &bis[i];

		shell_print(sh, "BIS %c     : sent %u, in controller %d, repeated %u, silence %u, "
			    "send err %u",
			    i == 0 ? 'L' : 'R', b->sent, (int)atomic_get(&b->inflight), b->repeated,
			    b->silenced, b->send_err);
	}
}

void bcast_source_log_status(void)
{
	LOG_INF("broadcast: %s, phone %s, sent %u/%u, queue %u, dropped %u, dry %u, repeated %u/%u",
		atomic_get(&running) ? "running" : (atomic_get(&paused) ? "paused (navigation)" : "DOWN"),
		atomic_get(&phone_active) ? "streaming" : "idle", bis[0].sent, bis[1].sent, q_cnt,
		dropped, dry, bis[0].repeated, bis[1].repeated);
}
