/*
 * Helmet - intercom buoc S2: duong am thanh giua hai mu.
 * Thiet ke: docs/HELMET_THIET_KE_INTERCOM.md muc 6.4
 *
 * MASTER (BAP Unicast Client) cau hinh 2 ASE cua SLAVE (ASCS san co, nhu dien thoai goi dien):
 *   ic_tx -> ASE sink cua slave  : giong master (micro -> DSP -> LC3) -> loa slave
 *   ic_rx <- ASE source cua slave: giong slave -> giai ma -> loa master
 * LC3 16 kHz mono 40 B / 10 ms tren 1 CIS hai chieu (QoS preset 16_2_1, PD 20 / 25 ms nam
 * trong khoang slave mong muon).
 * Chuoi: tim ASE sink, source -> Config Codec (2 stream) -> tao nhom -> QoS -> Enable
 *        -> tao CIS -> Receiver Start Ready (source) -> ca hai Streaming.
 * SLAVE khong can ma moi: le_audio.c / voice_tx.c / audio_out.c chay nhu khi dien thoai goi.
 *
 * Callback BAP (bt_rx) chi bao su kien qua intercom.c; buoc tiep theo chay trong thread bo dieu
 * khien (ic_audio_on_event) vi lenh BAP co the phai cho bo dem ATT.
 * Ket thuc: mat ACL -> Zephyr tu giai phong 2 stream -> xoa nhom.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "helmet.h"

LOG_MODULE_REGISTER(ic_audio, LOG_LEVEL_INF);

#define TX_BIT BIT(0)
#define RX_BIT BIT(1)

enum ae {
	AE_DISC_SNK,  /* arg = 1 neu loi */
	AE_DISC_SRC,  /* arg = 1 neu loi */
	AE_CONFIGURED,
	AE_QOS,
	AE_ENABLED,
	AE_ISO_CONNECTED,
	AE_STARTED,
	AE_STOPPED,
	AE_RELEASED,
	AE_ERROR, /* arg = ma phan hoi ASCS */
};

enum as {
	AS_IDLE,
	AS_DISC_SNK,
	AS_DISC_SRC,
	AS_CONFIG,
	AS_QOS,
	AS_ENABLE,
	AS_CONNECT,
	AS_START,
	AS_STREAMING,
};

static const char *const as_names[] = {"idle",   "discover sink", "discover source",
				       "config", "qos",           "enable",
				       "connect", "start",        "STREAMING"};

static struct bt_bap_lc3_preset preset_tx =
	BT_BAP_LC3_UNICAST_PRESET_16_2_1(BT_AUDIO_LOCATION_MONO_AUDIO,
					 BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);
static struct bt_bap_lc3_preset preset_rx =
	BT_BAP_LC3_UNICAST_PRESET_16_2_1(BT_AUDIO_LOCATION_MONO_AUDIO,
					 BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

static const uint8_t ic_meta[] = {
	BT_AUDIO_CODEC_DATA(BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT,
			    BT_BYTES_LIST_LE16(BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)),
};

/* Cau hinh dua cho voice_tx / audio_out: dung voi preset 16_2_1 mono */
static const struct helmet_stream_cfg voice_cfg = {
	.freq_hz = 16000,
	.frame_dur_us = 10000,
	.octets_per_frame = 40,
	.chan_cnt = 1,
	.chan_alloc = 0,
};

static struct bt_bap_stream ic_tx;
static struct bt_bap_stream ic_rx;
static struct bt_bap_ep *snk_ep; /* ghi trong callback endpoint khi dang tim */
static struct bt_bap_ep *src_ep;
static struct bt_bap_unicast_group *group;

/* Chi thread bo dieu khien */
static enum as state = AS_IDLE;
static struct bt_conn *a_conn;
static uint8_t done;     /* stream da xong buoc hien tai (TX_BIT / RX_BIT) */
static uint8_t started;  /* stream dang phat that (da goi voice_start / audio_out) */
static uint8_t released; /* stream da duoc giai phong sau khi dung */
static uint32_t sessions;
static uint32_t rx_sdu;
static uint32_t rx_bad;

static uint8_t stream_bit(const struct bt_bap_stream *stream)
{
	return stream == &ic_tx ? TX_BIT : (stream == &ic_rx ? RX_BIT : 0);
}

/* ---------------- Callback BAP (bt_rx) ---------------- */

static void endpoint_cb(struct bt_conn *conn, enum bt_audio_dir dir, struct bt_bap_ep *ep)
{
	ARG_UNUSED(conn);
	if (dir == BT_AUDIO_DIR_SINK && snk_ep == NULL) {
		snk_ep = ep;
	} else if (dir == BT_AUDIO_DIR_SOURCE && src_ep == NULL) {
		src_ep = ep;
	}
}

static void discover_cb(struct bt_conn *conn, int err, enum bt_audio_dir dir)
{
	ARG_UNUSED(conn);
	intercom_on_audio(dir == BT_AUDIO_DIR_SINK ? AE_DISC_SNK : AE_DISC_SRC, err ? 1 : 0);
}

static void rsp_cb(const char *op, struct bt_bap_stream *stream, enum bt_bap_ascs_rsp_code rsp,
		   enum bt_bap_ascs_reason reason)
{
	if (stream_bit(stream) == 0 || rsp == BT_BAP_ASCS_RSP_CODE_SUCCESS) {
		return;
	}
	LOG_WRN("%s %s rejected by peer: code 0x%02x reason 0x%02x", op,
		stream == &ic_tx ? "TX" : "RX", rsp, reason);
	intercom_on_audio(AE_ERROR, (uint8_t)rsp);
}

#define RSP_CB(name, op)                                                                            \
	static void name(struct bt_bap_stream *stream, enum bt_bap_ascs_rsp_code rsp,              \
			 enum bt_bap_ascs_reason reason)                                           \
	{                                                                                          \
		rsp_cb(op, stream, rsp, reason);                                                   \
	}

RSP_CB(config_rsp, "Config")
RSP_CB(qos_rsp, "QoS")
RSP_CB(enable_rsp, "Enable")
RSP_CB(start_rsp, "Start")

static struct bt_bap_unicast_client_cb client_cb = {
	.endpoint = endpoint_cb,
	.discover = discover_cb,
	.config = config_rsp,
	.qos = qos_rsp,
	.enable = enable_rsp,
	.start = start_rsp,
};

static void configured_cb(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg_pref *pref)
{
	ARG_UNUSED(pref);
	intercom_on_audio(AE_CONFIGURED, stream_bit(stream));
}

static void qos_set_cb(struct bt_bap_stream *stream)
{
	intercom_on_audio(AE_QOS, stream_bit(stream));
}

static void enabled_cb(struct bt_bap_stream *stream)
{
	intercom_on_audio(AE_ENABLED, stream_bit(stream));
}

static void connected_cb(struct bt_bap_stream *stream)
{
	intercom_on_audio(AE_ISO_CONNECTED, stream_bit(stream));
}

static void started_cb(struct bt_bap_stream *stream)
{
	intercom_on_audio(AE_STARTED, stream_bit(stream));
}

static void stopped_cb(struct bt_bap_stream *stream, uint8_t reason)
{
	LOG_INF("Intercom %s stream stopped (0x%02x)", stream == &ic_tx ? "TX" : "RX", reason);
	intercom_on_audio(AE_STOPPED, stream_bit(stream));
}

static void released_cb(struct bt_bap_stream *stream)
{
	intercom_on_audio(AE_RELEASED, stream_bit(stream));
}

static void recv_cb(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
		    struct net_buf *buf)
{
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0 && buf->len > 0;

	ARG_UNUSED(stream);
	rx_sdu++;
	if (!valid) {
		rx_bad++;
	}
	audio_out_stream_data(0, valid, info->ts, buf->data, buf->len);
}

static void sent_cb(struct bt_bap_stream *stream)
{
	voice_on_sent(stream);
}

static struct bt_bap_stream_ops stream_ops = {
	.configured = configured_cb,
	.qos_set = qos_set_cb,
	.enabled = enabled_cb,
	.connected = connected_cb,
	.started = started_cb,
	.stopped = stopped_cb,
	.released = released_cb,
	.recv = recv_cb,
	.sent = sent_cb,
};

/* ---------------- Thread bo dieu khien ---------------- */

static void stop_local(uint8_t bits)
{
	if ((bits & TX_BIT) && (started & TX_BIT)) {
		voice_stop();
	}
	if ((bits & RX_BIT) && (started & RX_BIT)) {
		audio_out_stream_stop(0);
	}
	started &= (uint8_t)~bits;
}

static void try_delete_group(void)
{
	if (group != NULL && bt_bap_unicast_group_delete(group) == 0) {
		group = NULL;
	}
}

static void fail(const char *why, int err)
{
	LOG_ERR("Intercom audio setup failed at %s (%d)", why, err);
	stop_local(TX_BIT | RX_BIT);
	(void)bt_bap_stream_release(&ic_tx);
	(void)bt_bap_stream_release(&ic_rx);
	state = AS_IDLE;
	audio_out_prompt(PROMPT_IC_UNAVAIL);
}

int ic_audio_start(struct bt_conn *conn)
{
	int err;

	if (state != AS_IDLE) {
		return -EBUSY;
	}
	if (a_conn != NULL) {
		bt_conn_unref(a_conn);
	}
	try_delete_group(); /* con sot tu lan truoc (vd mat song) */
	a_conn = bt_conn_ref(conn);
	snk_ep = NULL;
	src_ep = NULL;
	done = 0;
	released = 0;
	rx_sdu = 0;
	rx_bad = 0;
	sessions++;

	LOG_INF("Intercom audio: discovering peer ASEs");
	state = AS_DISC_SNK;
	err = bt_bap_unicast_client_discover(conn, BT_AUDIO_DIR_SINK);
	if (err) {
		fail("discover", err);
	}
	return err;
}

void ic_audio_on_event(uint8_t ev, uint8_t arg)
{
	int err = 0;

	switch (ev) {
	case AE_DISC_SNK:
		if (state != AS_DISC_SNK) {
			return;
		}
		if (arg || snk_ep == NULL) {
			fail("sink ASE discovery", -ENOENT);
			return;
		}
		state = AS_DISC_SRC;
		err = bt_bap_unicast_client_discover(a_conn, BT_AUDIO_DIR_SOURCE);
		if (err) {
			fail("source discovery", err);
		}
		return;

	case AE_DISC_SRC:
		if (state != AS_DISC_SRC) {
			return;
		}
		if (arg || src_ep == NULL) {
			fail("source ASE discovery", -ENOENT);
			return;
		}
		state = AS_CONFIG;
		done = 0;
		err = bt_bap_stream_config(a_conn, &ic_tx, snk_ep, &preset_tx.codec_cfg);
		if (!err) {
			err = bt_bap_stream_config(a_conn, &ic_rx, src_ep, &preset_rx.codec_cfg);
		}
		if (err) {
			fail("config", err);
		}
		return;

	case AE_CONFIGURED:
		if (state != AS_CONFIG || (done |= arg) != (TX_BIT | RX_BIT)) {
			return;
		}
		if (group == NULL) {
			struct bt_bap_unicast_group_stream_param tx_param = {
				.stream = &ic_tx,
				.qos = &preset_tx.qos,
			};
			struct bt_bap_unicast_group_stream_param rx_param = {
				.stream = &ic_rx,
				.qos = &preset_rx.qos,
			};
			/* Mot cap rx + tx -> mot CIS hai chieu */
			struct bt_bap_unicast_group_stream_pair_param pair = {
				.rx_param = &rx_param,
				.tx_param = &tx_param,
			};
			struct bt_bap_unicast_group_param param = {
				.params_count = 1,
				.params = &pair,
				.packing = BT_ISO_PACKING_SEQUENTIAL,
			};

			err = bt_bap_unicast_group_create(&param, &group);
			if (err) {
				fail("group create", err);
				return;
			}
		}
		state = AS_QOS;
		done = 0;
		err = bt_bap_stream_qos(a_conn, group);
		if (err) {
			fail("qos", err);
		}
		return;

	case AE_QOS:
		if (state != AS_QOS || (done |= arg) != (TX_BIT | RX_BIT)) {
			return;
		}
		state = AS_ENABLE;
		done = 0;
		err = bt_bap_stream_enable(&ic_tx, ic_meta, sizeof(ic_meta));
		if (!err) {
			err = bt_bap_stream_enable(&ic_rx, ic_meta, sizeof(ic_meta));
		}
		if (err) {
			fail("enable", err);
		}
		return;

	case AE_ENABLED:
		if (state != AS_ENABLE || (done |= arg) != (TX_BIT | RX_BIT)) {
			return;
		}
		state = AS_CONNECT;
		/* Hai stream chung 1 CIS: chi can tao 1 lan */
		err = bt_bap_stream_connect(&ic_tx);
		if (err) {
			fail("CIS connect", err);
		}
		return;

	case AE_ISO_CONNECTED:
		if (state != AS_CONNECT) {
			return; /* stream thu hai cua cung CIS */
		}
		state = AS_START;
		done = 0;
		/* Master la ben NHAN cua ASE source -> gui Receiver Start Ready. ASE sink do slave
		 * tu bao san sang (le_audio.c phia slave).
		 */
		err = bt_bap_stream_start(&ic_rx);
		if (err) {
			fail("start", err);
		}
		return;

	case AE_STARTED:
		if (state != AS_START && state != AS_STREAMING) {
			return;
		}
		if ((arg & TX_BIT) && !(started & TX_BIT)) {
			started |= TX_BIT;
			voice_start(&ic_tx, &voice_cfg);
		}
		if ((arg & RX_BIT) && !(started & RX_BIT)) {
			started |= RX_BIT;
			audio_out_stream_start(0, &voice_cfg);
		}
		if (started == (TX_BIT | RX_BIT) && state != AS_STREAMING) {
			state = AS_STREAMING;
			LOG_INF("Intercom audio STREAMING (LC3 16 kHz mono, both directions)");
		}
		return;

	case AE_STOPPED:
		stop_local(arg);
		return;

	case AE_RELEASED:
		stop_local(arg);
		released |= arg;
		if (released == (TX_BIT | RX_BIT)) {
			try_delete_group();
			if (state != AS_IDLE) {
				LOG_INF("Intercom audio released by peer");
				state = AS_IDLE;
			}
		}
		return;

	case AE_ERROR:
		if (state != AS_IDLE && state != AS_STREAMING) {
			fail("peer response", -EPROTO);
		}
		return;

	default:
		return;
	}
}

void ic_audio_conn_lost(void)
{
	stop_local(TX_BIT | RX_BIT);
	state = AS_IDLE;
	if (a_conn != NULL) {
		bt_conn_unref(a_conn);
		a_conn = NULL;
	}
	/* Zephyr giai phong 2 stream khi mat ACL; xoa nhom ngay neu duoc, neu khong thi o
	 * AE_RELEASED / lan bat dau sau
	 */
	try_delete_group();
}

bool ic_audio_streaming(void)
{
	return state == AS_STREAMING;
}

const char *ic_audio_state_str(void)
{
	return as_names[state];
}

void ic_audio_print(const struct shell *sh)
{
	shell_print(sh, "Audio     : %s (sessions %u), rx SDU %u (bad %u)", as_names[state],
		    sessions, rx_sdu, rx_bad);
}

int ic_audio_init(void)
{
	int err;

	/* PD nam trong khoang slave mong muon (le_audio.c: sink 20-40 ms, source 25-40 ms) */
	preset_tx.qos.pd = 20000U;
	preset_rx.qos.pd = 25000U;

	err = bt_bap_unicast_client_register_cb(&client_cb);
	if (err) {
		LOG_ERR("Unicast client register failed (%d)", err);
		return err;
	}
	bt_bap_stream_cb_register(&ic_tx, &stream_ops);
	bt_bap_stream_cb_register(&ic_rx, &stream_ops);
	return 0;
}
