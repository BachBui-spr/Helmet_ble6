/*
 * Helmet - LE Audio: BAP Unicast Server
 *   - 2 ASE sink  (dien thoai -> mu): nhac, giong nguoi ben kia, chuong
 *   - 1 ASE source (mu -> dien thoai): micro khi goi dien (ngu canh Conversational)
 *
 * Dien thoai doc PACS de biet Helmet nhan/gui duoc gi, roi cau hinh ASE
 * (Codec -> QoS -> Enable). ASE sink: Helmet tu bao "Receiver Start Ready".
 * ASE source: dien thoai gui Receiver Start Ready (lc3_start) roi stream_started -> voice_start.
 * Moi SDU nhan duoc chuyen cho audio_out (phat) va capture (gui ve PC).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "helmet.h"

LOG_MODULE_REGISTER(le_audio, LOG_LEVEL_INF);

/* Chieu nhan: LC3 16-48 kHz, 7.5/10 ms, 1-2 kenh, 26-155 octet */
static const struct bt_audio_codec_cap lc3_snk_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |
		BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_7_5 | BT_AUDIO_CODEC_CAP_DURATION_10,
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1, 2), 26u, 155u, 1u, BT_AUDIO_CONTEXT_TYPE_MEDIA);

/* Chieu gui (micro): 16_2 / 32_2 - 1 kenh, chi khung 10 ms (loi DSP lam viec theo 10 ms) */
static const struct bt_audio_codec_cap lc3_src_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_32KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 40u, 80u, 1u,
	BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

static struct bt_pacs_cap snk_cap = {
	.codec_cap = &lc3_snk_cap,
};

static struct bt_pacs_cap src_cap = {
	.codec_cap = &lc3_src_cap,
};

/* Uu tien QoS chieu nhan: unframed, 2M, RTN 2, tre 10 ms, presentation delay 20-40 ms.
 * Doi duoc bang "helmet cispref" (ap dung tu lan Config Codec ke tiep).
 */
static struct bt_bap_qos_cfg_pref qos_pref =
	BT_BAP_QOS_CFG_PREF(true, BT_GAP_LE_PHY_2M, 0x02, 10, 20000, 40000, 20000, 40000);

/* Chieu gui: PD toi thieu 25 ms = khoi PDM 10 ms + xu ly + cho trong hang SDU */
static const struct bt_bap_qos_cfg_pref qos_pref_src =
	BT_BAP_QOS_CFG_PREF(true, BT_GAP_LE_PHY_2M, 0x02, 10, 25000, 40000, 25000, 40000);

static struct helmet_rx_flags rx_flags;

struct helmet_stream {
	struct bt_bap_stream stream;
	struct helmet_stream_cfg cfg;
	bool streaming;
	uint32_t rx_count;
};

static struct helmet_stream streams[HELMET_MAX_STREAMS];
static struct helmet_stream src;

static bool is_src(const struct bt_bap_stream *stream)
{
	return stream == &src.stream;
}

static uint8_t stream_idx(const struct bt_bap_stream *stream)
{
	return (uint8_t)(CONTAINER_OF(stream, struct helmet_stream, stream) - streams);
}

static const char *stream_name(const struct bt_bap_stream *stream, char *buf, size_t len)
{
	if (is_src(stream)) {
		return "SRC";
	}
	snprintk(buf, len, "ASE %u", stream_idx(stream));
	return buf;
}

/* Doc cau hinh LC3 ma dien thoai chon, kiem tra Helmet ho tro duoc */
static int parse_codec_cfg(const struct bt_audio_codec_cfg *codec_cfg, enum bt_audio_dir dir,
			   struct helmet_stream_cfg *out)
{
	enum bt_audio_location alloc;
	int ret;

	if (codec_cfg->id != BT_HCI_CODING_FORMAT_LC3) {
		return -ENOTSUP;
	}

	ret = bt_audio_codec_cfg_get_freq(codec_cfg);
	if (ret < 0) {
		return ret;
	}
	out->freq_hz = bt_audio_codec_cfg_freq_to_freq_hz(ret);

	ret = bt_audio_codec_cfg_get_frame_dur(codec_cfg);
	if (ret < 0) {
		return ret;
	}
	out->frame_dur_us = bt_audio_codec_cfg_frame_dur_to_frame_dur_us(ret);

	ret = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);
	if (ret <= 0) {
		return -EINVAL;
	}
	out->octets_per_frame = (uint16_t)ret;

	/* Khong co Audio Location -> mac dinh mono */
	ret = bt_audio_codec_cfg_get_chan_allocation(codec_cfg, &alloc, true);
	if (ret < 0) {
		return ret;
	}
	out->chan_alloc = alloc;
	out->chan_cnt = bt_audio_get_chan_count(alloc);

	ret = bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true);
	if (ret != 1) {
		return -ENOTSUP; /* PACS chi cho phep 1 khung/SDU */
	}

	if (dir == BT_AUDIO_DIR_SOURCE) {
		if (out->chan_cnt != 1 || out->frame_dur_us != 10000 ||
		    (out->freq_hz != 16000 && out->freq_hz != 32000) ||
		    out->octets_per_frame < 40 || out->octets_per_frame > 80) {
			return -ENOTSUP;
		}
		return 0;
	}

	if (out->chan_cnt > 2 || out->octets_per_frame * out->chan_cnt > HELMET_MAX_SDU_LEN) {
		return -ENOTSUP;
	}

	return 0;
}

static void log_cfg(const char *what, const char *name, const struct helmet_stream_cfg *cfg)
{
	LOG_INF("%s %s: LC3 %u Hz, %u us, %u ch (loc 0x%x), %u B/frame", what, name, cfg->freq_hz,
		cfg->frame_dur_us, cfg->chan_cnt, cfg->chan_alloc, cfg->octets_per_frame);
}

/* ---------------- ASCS callbacks (dien thoai dieu khien trang thai ASE) ---------------- */

static int lc3_config(struct bt_conn *conn, const struct bt_bap_ep *ep, enum bt_audio_dir dir,
		      const struct bt_audio_codec_cfg *codec_cfg, struct bt_bap_stream **stream,
		      struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	struct helmet_stream_cfg cfg;
	char name[12];

	if (parse_codec_cfg(codec_cfg, dir, &cfg) != 0) {
		LOG_WRN("Codec config rejected (%s, codec 0x%02x)",
			dir == BT_AUDIO_DIR_SOURCE ? "source" : "sink", codec_cfg->id);
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -ENOTSUP;
	}

	if (dir == BT_AUDIO_DIR_SOURCE) {
		if (src.stream.conn != NULL) {
			*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM, BT_BAP_ASCS_REASON_NONE);
			return -ENOMEM;
		}
		*stream = &src.stream;
		*pref = qos_pref_src;
		log_cfg("Config", "SRC", &cfg);
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i].stream.conn == NULL) {
			*stream = &streams[i].stream;
			*pref = qos_pref;
			log_cfg("Config", stream_name(*stream, name, sizeof(name)), &cfg);
			return 0;
		}
	}

	LOG_WRN("No free stream");
	*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM, BT_BAP_ASCS_REASON_NONE);
	return -ENOMEM;
}

static int lc3_reconfig(struct bt_bap_stream *stream, enum bt_audio_dir dir,
			const struct bt_audio_codec_cfg *codec_cfg,
			struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	struct helmet_stream_cfg cfg;
	char name[12];

	if (parse_codec_cfg(codec_cfg, dir, &cfg) != 0) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -ENOTSUP;
	}

	log_cfg("Reconfig", stream_name(stream, name, sizeof(name)), &cfg);
	*pref = (dir == BT_AUDIO_DIR_SOURCE) ? qos_pref_src : qos_pref;
	return 0;
}

static int lc3_qos(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg *qos,
		   struct bt_bap_ascs_rsp *rsp)
{
	char name[12];

	LOG_INF("QoS %s: interval %u us, %s, PHY %s, SDU %u B, RTN %u, latency %u ms, PD %u us",
		stream_name(stream, name, sizeof(name)), qos->interval,
		qos->framing == BT_BAP_QOS_CFG_FRAMING_FRAMED ? "FRAMED" : "unframed",
		le_audio_phy_str(qos->phy), qos->sdu, qos->rtn, qos->latency, qos->pd);
	return 0;
}

static int lc3_enable(struct bt_bap_stream *stream, const uint8_t meta[], size_t meta_len,
		      struct bt_bap_ascs_rsp *rsp)
{
	struct helmet_stream *s = CONTAINER_OF(stream, struct helmet_stream, stream);
	const enum bt_audio_dir dir = is_src(stream) ? BT_AUDIO_DIR_SOURCE : BT_AUDIO_DIR_SINK;
	char name[12];

	if (parse_codec_cfg(stream->codec_cfg, dir, &s->cfg) != 0) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_INVALID,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -EINVAL;
	}

	LOG_INF("Enable %s (metadata %zu B)", stream_name(stream, name, sizeof(name)), meta_len);
	return 0;
}

static int lc3_start(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	char name[12];

	/* Chi duoc goi cho ASE source: dien thoai da san sang nhan giong noi */
	LOG_INF("Receiver Start Ready %s", stream_name(stream, name, sizeof(name)));
	return 0;
}

static bool metadata_check_cb(struct bt_data *data, void *user_data)
{
	struct bt_bap_ascs_rsp *rsp = user_data;

	if (!BT_AUDIO_METADATA_TYPE_IS_KNOWN(data->type)) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_METADATA_REJECTED, data->type);
		return false;
	}
	return true;
}

static int lc3_metadata(struct bt_bap_stream *stream, const uint8_t meta[], size_t meta_len,
			struct bt_bap_ascs_rsp *rsp)
{
	return bt_audio_data_parse(meta, meta_len, metadata_check_cb, rsp);
}

static int lc3_disable(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	char name[12];

	LOG_INF("Disable %s", stream_name(stream, name, sizeof(name)));
	return 0;
}

static int lc3_stop(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	char name[12];

	LOG_INF("Stop %s", stream_name(stream, name, sizeof(name)));
	return 0;
}

static int lc3_release(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	char name[12];

	LOG_INF("Release %s", stream_name(stream, name, sizeof(name)));
	return 0;
}

static const struct bt_bap_unicast_server_cb unicast_server_cb = {
	.config = lc3_config,
	.reconfig = lc3_reconfig,
	.qos = lc3_qos,
	.enable = lc3_enable,
	.start = lc3_start,
	.metadata = lc3_metadata,
	.disable = lc3_disable,
	.stop = lc3_stop,
	.release = lc3_release,
};

/* ---------------- Stream callbacks (luong CIS) ---------------- */

static void stream_enabled(struct bt_bap_stream *stream)
{
	struct bt_bap_ep_info ep_info;
	int err;

	if (bt_bap_ep_get_info(stream->ep, &ep_info) != 0 || ep_info.dir != BT_AUDIO_DIR_SINK) {
		return; /* ASE source: cho dien thoai gui Receiver Start Ready */
	}

	/* Voi ASE sink, phia server (Helmet) bao "Receiver Start Ready" */
	err = bt_bap_stream_start(stream);
	if (err) {
		LOG_ERR("Start ASE %u failed (%d)", stream_idx(stream), err);
	}
}

/* Thong so CIS ma dien thoai (central) da chon. sh == NULL -> ghi log */
static void print_cis_info(const struct shell *sh, const char *name, struct bt_bap_stream *stream)
{
	struct bt_bap_ep_info ep_info;
	struct bt_iso_info info;
	const struct bt_iso_unicast_tx_info *c2p;
	char line[200];

	if (stream->ep == NULL || bt_bap_ep_get_info(stream->ep, &ep_info) != 0 ||
	    ep_info.iso_chan == NULL || bt_iso_chan_get_info(ep_info.iso_chan, &info) != 0) {
		snprintk(line, sizeof(line), "CIS %s: info not available", name);
	} else {
		c2p = &info.unicast.central;
		snprintk(line, sizeof(line),
			 "CIS %s: ISO interval %u us, NSE %u, subinterval %u us, CIG sync %u us, "
			 "C->P PHY %s, BN %u, FT %u, max PDU %u B, max SDU %u B, latency %u us",
			 name, info.iso_interval * 1250U, info.max_subevent,
			 info.unicast.subinterval, info.unicast.cig_sync_delay,
			 le_audio_phy_str(c2p->phy), c2p->bn,
			 /* Zephyr luu flush_timeout theo don vi 1.25 ms */
			 info.iso_interval ? c2p->flush_timeout / info.iso_interval : 0U,
			 c2p->max_pdu, c2p->max_sdu, c2p->latency);
	}

	if (sh != NULL) {
		shell_print(sh, "%s", line);
	} else {
		LOG_INF("%s", line);
	}
}

/* Bo dem cua controller cho CIS (tinh tu luc CIS bat dau), o muc goi vo tuyen */
static int read_iso_link_quality(struct bt_bap_stream *stream,
				 struct bt_hci_rp_le_read_iso_link_quality *lq)
{
	struct bt_hci_cp_le_read_iso_link_quality *cp;
	struct bt_bap_ep_info ep_info;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	uint16_t handle;
	int err;

	if (stream->ep == NULL || bt_bap_ep_get_info(stream->ep, &ep_info) != 0 ||
	    ep_info.iso_chan == NULL || ep_info.iso_chan->iso == NULL) {
		return -ENOTCONN;
	}

	err = bt_hci_get_conn_handle(ep_info.iso_chan->iso, &handle);
	if (err) {
		return err;
	}

	buf = bt_hci_cmd_alloc(K_MSEC(100));
	if (buf == NULL) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_ISO_LINK_QUALITY, buf, &rsp);
	if (err) {
		return err;
	}

	memset(lq, 0, sizeof(*lq));
	memcpy(lq, rsp->data, MIN(rsp->len, sizeof(*lq)));
	net_buf_unref(rsp);
	return lq->status ? -EIO : 0;
}

/* sh == NULL -> ghi log (heartbeat), nguoc lai in ra shell */
void le_audio_print_link_quality(const struct shell *sh)
{
	struct bt_hci_rp_le_read_iso_link_quality lq;
	char line[160];

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		int err;

		if (!streams[i].streaming) {
			continue;
		}

		err = read_iso_link_quality(&streams[i].stream, &lq);
		if (err) {
			snprintk(line, sizeof(line), "ISO LQ ASE %u: read failed (%d)", i, err);
		} else {
			snprintk(line, sizeof(line),
				 "ISO LQ ASE %u (goi vo tuyen): CRC error %u, unreceived %u, "
				 "duplicate %u, tx flushed %u",
				 i, sys_le32_to_cpu(lq.crc_error_packets),
				 sys_le32_to_cpu(lq.rx_unreceived_packets),
				 sys_le32_to_cpu(lq.duplicate_packets),
				 sys_le32_to_cpu(lq.tx_flushed_packets));
		}

		if (sh != NULL) {
			shell_print(sh, "%s", line);
		} else {
			LOG_INF("%s", line);
		}
	}
}

static void stream_started(struct bt_bap_stream *stream)
{
	struct helmet_stream *s = CONTAINER_OF(stream, struct helmet_stream, stream);
	char name[12];

	s->streaming = true;
	s->rx_count = 0;
	stream_name(stream, name, sizeof(name));
	log_cfg("Streaming", name, &s->cfg);
	print_cis_info(NULL, name, stream);

	if (is_src(stream)) {
		voice_start(stream, &s->cfg);
		app_post(EVT_STREAM, BT_AUDIO_DIR_SOURCE, 1, 0);
		return;
	}

	audio_out_stream_start(stream_idx(stream), &s->cfg);
	capture_stream_start(stream_idx(stream), &s->cfg);
	app_post(EVT_STREAM, BT_AUDIO_DIR_SINK, 1, 0);
}

static void stream_end(struct bt_bap_stream *stream, const char *why, uint8_t reason)
{
	struct helmet_stream *s = CONTAINER_OF(stream, struct helmet_stream, stream);
	char name[12];

	if (!s->streaming) {
		return;
	}
	s->streaming = false;
	LOG_INF("%s %s (reason 0x%02x, %u SDU)", stream_name(stream, name, sizeof(name)), why,
		reason, s->rx_count);

	if (is_src(stream)) {
		voice_stop();
		app_post(EVT_STREAM, BT_AUDIO_DIR_SOURCE, 0, 0);
		return;
	}

	audio_out_stream_stop(stream_idx(stream));
	capture_stream_stop(stream_idx(stream));
	app_post(EVT_STREAM, BT_AUDIO_DIR_SINK, 0, 0);
}

static void stream_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	stream_end(stream, "stopped", reason);
}

static void stream_released(struct bt_bap_stream *stream)
{
	stream_end(stream, "released", 0);
}

static void stream_recv(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	struct helmet_stream *s = CONTAINER_OF(stream, struct helmet_stream, stream);
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0;

	if (is_src(stream)) {
		return;
	}

	s->rx_count++;
	if (valid && buf->len > 0) {
		rx_flags.ok++;
	} else if (info->flags & BT_ISO_FLAGS_LOST) {
		rx_flags.lost++; /* khong nhan duoc PDU nao trong cac su kien CIS */
	} else if (info->flags & BT_ISO_FLAGS_ERROR) {
		rx_flags.crc++; /* nhan duoc nhung du lieu co the hong */
	} else {
		rx_flags.empty++; /* SDU do dai 0 */
	}
	audio_out_stream_data(stream_idx(stream), valid && buf->len > 0, info->ts, buf->data,
			      buf->len);
	capture_sdu(stream_idx(stream), info->flags, info->seq_num, info->ts, buf->data, buf->len);
}

static void stream_sent(struct bt_bap_stream *stream)
{
	if (is_src(stream)) {
		src.rx_count++; /* voi ASE source: dem SDU da gui */
		voice_on_sent(stream);
	}
}

static struct bt_bap_stream_ops stream_ops = {
	.enabled = stream_enabled,
	.started = stream_started,
	.stopped = stream_stopped,
	.released = stream_released,
	.recv = stream_recv,
	.sent = stream_sent,
};

/* ---------------- API ---------------- */

int le_audio_init(void)
{
	static struct bt_bap_unicast_server_register_param param = {
		.snk_cnt = CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT,
		.src_cnt = CONFIG_BT_ASCS_MAX_ASE_SRC_COUNT,
	};
	const struct bt_pacs_register_param pacs_param = {
		.snk_pac = true,
		.snk_loc = true,
		.src_pac = true,
		.src_loc = true,
	};
	int err;

	err = bt_pacs_register(&pacs_param);
	if (err) {
		LOG_ERR("PACS register failed (%d)", err);
		return err;
	}

	err = bt_bap_unicast_server_register(&param);
	if (err) {
		LOG_ERR("Unicast server register failed (%d)", err);
		return err;
	}

	err = bt_bap_unicast_server_register_cb(&unicast_server_cb);
	if (err) {
		LOG_ERR("Unicast server callbacks failed (%d)", err);
		return err;
	}

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &snk_cap);
	if (err) {
		LOG_ERR("PACS sink capability register failed (%d)", err);
		return err;
	}

	err = bt_pacs_cap_register(BT_AUDIO_DIR_SOURCE, &src_cap);
	if (err) {
		LOG_ERR("PACS source capability register failed (%d)", err);
		return err;
	}

	(void)bt_pacs_set_location(BT_AUDIO_DIR_SINK, HELMET_SINK_LOCATION);
	(void)bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SINK, HELMET_SINK_CONTEXTS);
	(void)bt_pacs_set_available_contexts(BT_AUDIO_DIR_SINK, HELMET_SINK_CONTEXTS);

	(void)bt_pacs_set_location(BT_AUDIO_DIR_SOURCE, HELMET_SOURCE_LOCATION);
	(void)bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SOURCE, HELMET_SOURCE_CONTEXTS);
	(void)bt_pacs_set_available_contexts(BT_AUDIO_DIR_SOURCE, HELMET_SOURCE_CONTEXTS);

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		bt_bap_stream_cb_register(&streams[i].stream, &stream_ops);
	}
	bt_bap_stream_cb_register(&src.stream, &stream_ops);

	LOG_INF("BAP unicast server: %u sink ASE (LC3 16-48 kHz, 1-2 ch) + %u source ASE "
		"(LC3 16/32 kHz mono)",
		CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT, CONFIG_BT_ASCS_MAX_ASE_SRC_COUNT);
	return 0;
}

void le_audio_set_source_available(bool available)
{
	int err = bt_pacs_set_available_contexts(BT_AUDIO_DIR_SOURCE,
						 available ? HELMET_SOURCE_CONTEXTS : 0);

	if (err) {
		LOG_WRN("Set source available contexts failed (%d)", err);
	} else {
		LOG_INF("Microphone %s for calls", available ? "available" : "NOT available");
	}
}

const char *le_audio_phy_str(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1M";
	case BT_GAP_LE_PHY_2M:
		return "2M";
	case BT_GAP_LE_PHY_CODED:
		return "Coded";
	default:
		return "?";
	}
}

void le_audio_set_qos_pref(uint8_t phy, uint8_t rtn, uint16_t latency_ms)
{
	qos_pref.phy = phy;
	qos_pref.rtn = rtn;
	qos_pref.latency = latency_ms;
	LOG_INF("Preferred CIS QoS: PHY %s, RTN %u, latency %u ms (applies from next codec config)",
		le_audio_phy_str(phy), rtn, latency_ms);
}

void le_audio_get_rx_flags(struct helmet_rx_flags *out)
{
	*out = rx_flags;
}

bool le_audio_is_streaming(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i].streaming) {
			return true;
		}
	}
	return false;
}

bool le_audio_source_streaming(void)
{
	return src.streaming;
}

void le_audio_print_status(const struct shell *sh)
{
	struct helmet_audio_stats st;

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		const struct helmet_stream *s = &streams[i];

		if (s->stream.conn == NULL) {
			shell_print(sh, "ASE %u     : free", i);
		} else {
			char name[12];

			shell_print(sh, "ASE %u     : %s, LC3 %u Hz %u us %u ch loc 0x%x %u B, rx %u",
				    i, s->streaming ? "STREAMING" : "configured", s->cfg.freq_hz,
				    s->cfg.frame_dur_us, s->cfg.chan_cnt, s->cfg.chan_alloc,
				    s->cfg.octets_per_frame, s->rx_count);
			if (s->streaming) {
				snprintk(name, sizeof(name), "ASE %u", i);
				print_cis_info(sh, name, (struct bt_bap_stream *)&s->stream);
			}
		}
	}
	if (src.stream.conn == NULL) {
		shell_print(sh, "SRC       : free");
	} else {
		shell_print(sh, "SRC       : %s, LC3 %u Hz %u us %u B, sent %u",
			    src.streaming ? "STREAMING" : "configured", src.cfg.freq_hz,
			    src.cfg.frame_dur_us, src.cfg.octets_per_frame, src.rx_count);
	}
	le_audio_print_link_quality(sh);

	shell_print(sh, "ISO rx    : ok %u, CRC error %u, lost %u, empty %u", rx_flags.ok,
		    rx_flags.crc, rx_flags.lost, rx_flags.empty);
	shell_print(sh, "QoS pref  : PHY %s, RTN %u, latency %u ms", le_audio_phy_str(qos_pref.phy),
		    qos_pref.rtn, qos_pref.latency);

	audio_out_get_stats(&st);
	shell_print(sh, "I2S       : %s, %u Hz (actual %u.%03u Hz), %u samples/frame, %u blocks, "
		    "ring %u",
		    st.i2s_running ? "running" : "stopped", st.out_rate, st.i2s_rate_mhz / 1000U,
		    st.i2s_rate_mhz % 1000U, st.frame_samples, st.queued_blocks, st.ring_level);
	shell_print(sh, "Channels  : %s%s%s", (st.active_mask & BIT(0)) ? "L " : "",
		    (st.active_mask & BIT(1)) ? "R " : "", st.prompt_active ? "+prompt" : "");
	shell_print(sh, "SDU       : rx %u, bad/lost %u, len err %u, queue full %u", st.sdu_rx,
		    st.sdu_bad, st.len_err, st.q_full);
	shell_print(sh, "Decoder   : PLC %u, errors %u", st.plc, st.dec_err);
	shell_print(sh, "Clock     : slip -%u/+%u samples, underrun %u, ring drop %u, I2S err %u",
		    st.slip_drop, st.slip_insert, st.underrun, st.ring_drop, st.i2s_err);
}
