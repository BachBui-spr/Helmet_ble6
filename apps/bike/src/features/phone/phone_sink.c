/*
 * Bike hub - phia dien thoai: BAP Unicast Server, chi chieu NHAN (hub khong co micro).
 *
 * Dien thoai doc PACS (chi LC3 48 kHz / 10 ms / 100-155 octet / 1-2 kenh) roi cau hinh ASE sink
 * (Config -> QoS -> Enable); hub bao "Receiver Start Ready". Moi SDU nhan qua CIS duoc tach
 * thanh cap khung trai / phai va chuyen nguyen ven sang bcast_source.c:
 *   1 ASE 2 kenh (FL|FR): SDU = [khung trai][khung phai]       -> ca hai BIS
 *   1 ASE 1 kenh (mono, hoac chi mot ben dang chay)              -> cung khung cho hai BIS
 *   2 ASE 1 kenh (FL va FR rieng): ghep theo moc thoi gian SDU   -> moi ben mot BIS
 * Moi callback chay trong bt_rx (thread duy nhat goi bcast_source_push_pair tu phia nay).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>

#include "features/broadcast/bcast_source.h"
#include "features/phone/phone_sink.h"

LOG_MODULE_REGISTER(phone_sink, LOG_LEVEL_INF);

/* Ngu canh hub nhan: nhac, tro choi, chi duong (Instructional). KHONG co Conversational /
 * Ringtone: hub khong co micro, cuoc goi o lai dien thoai.
 */
#define SINK_CONTEXTS                                                                              \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_MEDIA |                         \
	 BT_AUDIO_CONTEXT_TYPE_GAME | BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL)
#define SINK_LOCATION (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)

/* Chi 48 kHz / 10 ms: BIG phat lai dung cau hinh nay, khong chuyen ma */
static const struct bt_audio_codec_cap lc3_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_48KHZ, BT_AUDIO_CODEC_CAP_DURATION_10,
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1, 2), BIKE_OCTETS_MIN, BIKE_OCTETS_MAX, 1u,
	SINK_CONTEXTS);

static struct bt_pacs_cap cap = {
	.codec_cap = &lc3_cap,
};

/* Uu tien QoS: unframed, 2M, RTN 2, tre toi da 20 ms, presentation delay 20-40 ms.
 * Tre 20 ms (truoc 10 ms, do 23/10/2026: dien thoai lay dung gia tri hub de nghi): cho phep
 * controller dien thoai dung FT 2 - goi bi BIG 20 ms cua hub chan o mot su kien CIS duoc gui lai
 * o su kien ke tiep (bcast_source.c). Voi tre 10 ms la FT 1: goi bi chan la mat.
 */
static const struct bt_bap_qos_cfg_pref qos_pref =
	BT_BAP_QOS_CFG_PREF(true, BT_GAP_LE_PHY_2M, 0x02, 20, 20000, 40000, 20000, 40000);

struct sink_cfg {
	uint16_t octets;
	uint8_t chan_cnt;
	uint32_t alloc;
};

struct ph_stream {
	struct bt_bap_stream stream;
	struct sink_cfg cfg;
	uint8_t mask; /* ASE 1 kenh: ben nao (BIKE_CH_L / BIKE_CH_R / ca hai) */
	bool streaming;
	uint32_t rx;
	uint32_t bad;
	uint16_t run;     /* SDU mat lien tiep hien tai */
	uint16_t max_run; /* chuoi mat dai nhat tu lan heartbeat truoc */
	uint32_t l_rx;    /* gia tri o lan heartbeat truoc */
	uint32_t l_bad;
};

static struct ph_stream streams[CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT];

/* Ghep khung tu 2 ASE mono (bt_rx) */
static uint8_t pend[2][BIKE_OCTETS_MAX];
static uint8_t pend_mask;  /* kenh da co mat trong cap dang ghep */
static uint8_t pend_valid; /* kenh co du lieu hop le */
static uint32_t pend_ts;
static uint16_t pend_len;
static uint8_t expected; /* hop cac ben cua moi ASE 1 kenh dang chay */

static uint8_t stream_idx(const struct bt_bap_stream *stream)
{
	return (uint8_t)(CONTAINER_OF(stream, struct ph_stream, stream) - streams);
}

static int parse_cfg(const struct bt_audio_codec_cfg *c, struct sink_cfg *out)
{
	enum bt_audio_location alloc;
	int ret;

	if (c->id != BT_HCI_CODING_FORMAT_LC3) {
		return -ENOTSUP;
	}
	ret = bt_audio_codec_cfg_get_freq(c);
	if (ret < 0 || bt_audio_codec_cfg_freq_to_freq_hz(ret) != BIKE_FREQ_HZ) {
		return -ENOTSUP;
	}
	ret = bt_audio_codec_cfg_get_frame_dur(c);
	if (ret < 0 || bt_audio_codec_cfg_frame_dur_to_frame_dur_us(ret) != BIKE_FRAME_US) {
		return -ENOTSUP;
	}
	ret = bt_audio_codec_cfg_get_octets_per_frame(c);
	if (ret < BIKE_OCTETS_MIN || ret > BIKE_OCTETS_MAX) {
		return -ENOTSUP;
	}
	out->octets = (uint16_t)ret;
	ret = bt_audio_codec_cfg_get_chan_allocation(c, &alloc, true);
	if (ret < 0) {
		return ret;
	}
	out->alloc = alloc;
	out->chan_cnt = bt_audio_get_chan_count(alloc);
	if (out->chan_cnt == 0 || out->chan_cnt > 2) {
		return -ENOTSUP;
	}
	if (bt_audio_codec_cfg_get_frame_blocks_per_sdu(c, true) != 1) {
		return -ENOTSUP;
	}
	return 0;
}

static uint8_t mono_mask(uint32_t alloc)
{
	if ((alloc & BT_AUDIO_LOCATION_FRONT_LEFT) && !(alloc & BT_AUDIO_LOCATION_FRONT_RIGHT)) {
		return BIKE_CH_L;
	}
	if ((alloc & BT_AUDIO_LOCATION_FRONT_RIGHT) && !(alloc & BT_AUDIO_LOCATION_FRONT_LEFT)) {
		return BIKE_CH_R;
	}
	return BIKE_CH_L | BIKE_CH_R; /* mono: ca hai ben */
}

/* ---------------- ASCS callbacks ---------------- */

static int ascs_config(struct bt_conn *conn, const struct bt_bap_ep *ep, enum bt_audio_dir dir,
		       const struct bt_audio_codec_cfg *codec_cfg, struct bt_bap_stream **stream,
		       struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	struct sink_cfg cfg;

	ARG_UNUSED(conn);
	ARG_UNUSED(ep);

	if (dir != BT_AUDIO_DIR_SINK || parse_cfg(codec_cfg, &cfg) != 0) {
		LOG_WRN("Codec config rejected (only LC3 48 kHz / 10 ms / %u-%u B)", BIKE_OCTETS_MIN,
			BIKE_OCTETS_MAX);
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -ENOTSUP;
	}

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i].stream.conn == NULL) {
			*stream = &streams[i].stream;
			*pref = qos_pref;
			LOG_INF("Config ASE %u: LC3 48 kHz, %u ch (loc 0x%x), %u B/frame", (unsigned)i,
				cfg.chan_cnt, cfg.alloc, cfg.octets);
			return 0;
		}
	}
	*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM, BT_BAP_ASCS_REASON_NONE);
	return -ENOMEM;
}

static int ascs_reconfig(struct bt_bap_stream *stream, enum bt_audio_dir dir,
			 const struct bt_audio_codec_cfg *codec_cfg,
			 struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	struct sink_cfg cfg;

	if (dir != BT_AUDIO_DIR_SINK || parse_cfg(codec_cfg, &cfg) != 0) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -ENOTSUP;
	}
	LOG_INF("Reconfig ASE %u: %u ch, %u B/frame", stream_idx(stream), cfg.chan_cnt,
		cfg.octets);
	*pref = qos_pref;
	return 0;
}

static int ascs_qos(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg *qos,
		    struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(rsp);
	LOG_INF("QoS ASE %u: interval %u us, SDU %u B, RTN %u, latency %u ms, PD %u us",
		stream_idx(stream), qos->interval, qos->sdu, qos->rtn, qos->latency, qos->pd);
	return 0;
}

static int ascs_enable(struct bt_bap_stream *stream, const uint8_t meta[], size_t meta_len,
		       struct bt_bap_ascs_rsp *rsp)
{
	struct ph_stream *s = CONTAINER_OF(stream, struct ph_stream, stream);

	ARG_UNUSED(meta);
	if (parse_cfg(stream->codec_cfg, &s->cfg) != 0) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_INVALID,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -EINVAL;
	}
	LOG_INF("Enable ASE %u (metadata %zu B)", stream_idx(stream), meta_len);
	return 0;
}

static int ascs_start(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(stream);
	ARG_UNUSED(rsp);
	return 0; /* chi cho ASE source; hub khong co */
}

static int ascs_metadata(struct bt_bap_stream *stream, const uint8_t meta[], size_t meta_len,
			 struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(stream);
	ARG_UNUSED(meta);
	ARG_UNUSED(meta_len);
	ARG_UNUSED(rsp);
	return 0;
}

static int ascs_disable(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(rsp);
	LOG_INF("Disable ASE %u", stream_idx(stream));
	return 0;
}

static int ascs_stop(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(rsp);
	LOG_INF("Stop ASE %u", stream_idx(stream));
	return 0;
}

static int ascs_release(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(rsp);
	LOG_INF("Release ASE %u", stream_idx(stream));
	return 0;
}

static const struct bt_bap_unicast_server_cb ascs_cb = {
	.config = ascs_config,
	.reconfig = ascs_reconfig,
	.qos = ascs_qos,
	.enable = ascs_enable,
	.start = ascs_start,
	.metadata = ascs_metadata,
	.disable = ascs_disable,
	.stop = ascs_stop,
	.release = ascs_release,
};

/* ---------------- Luong CIS -> cap khung -> broadcast ---------------- */

static void update_expected(void)
{
	expected = 0;
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i].streaming && streams[i].cfg.chan_cnt == 1) {
			expected |= streams[i].mask;
		}
	}
}

static void flush_pair(void)
{
	if (pend_mask == 0) {
		return;
	}
	bcast_source_push_pair((pend_valid & BIKE_CH_L) ? pend[0] : NULL,
			       (pend_valid & BIKE_CH_R) ? pend[1] : NULL, pend_len);
	pend_mask = 0;
	pend_valid = 0;
}

/* Hai ASE 1 kenh (trai / phai rieng): ghep theo moc thoi gian SDU */
static void put_side(uint8_t mask, const uint8_t *frame, uint16_t len, uint32_t ts)
{
	if (pend_mask != 0 && (ts != pend_ts || len != pend_len)) {
		flush_pair(); /* sang khoang thoi gian moi: day cap cu (ben thieu = mat goi) */
	}
	pend_ts = ts;
	pend_len = len;
	for (uint8_t c = 0; c < 2; c++) {
		if (mask & BIT(c)) {
			pend_mask |= BIT(c);
			if (frame != NULL) {
				memcpy(pend[c], frame, len);
				pend_valid |= BIT(c);
			}
		}
	}
	if ((pend_mask & expected) == expected) {
		flush_pair();
	}
}

/* Tham so CIS dien thoai thuc su chon (huong dien thoai -> hub = "central"). FT = so su kien CIS
 * mot goi duoc gui lai; can >= 2 khi BIG 20 ms (bcast_source.c) de goi bi chan khong bi mat.
 */
static void log_cis_params(struct bt_bap_stream *stream)
{
	struct bt_iso_info info;
	uint32_t ft;

	if (stream->iso == NULL || bt_iso_chan_get_info(stream->iso, &info) != 0 ||
	    info.type != BT_ISO_CHAN_TYPE_PERIPHERAL || info.iso_interval == 0U) {
		return;
	}
	/* flush_timeout = FT x ISO interval (don vi 1,25 ms) */
	ft = info.unicast.central.flush_timeout / info.iso_interval;
	LOG_INF("Phone CIS: ISO interval %u us, NSE %u, BN %u, FT %u, PDU %u B, subinterval %u us, "
		"transport latency %u us",
		info.iso_interval * 1250U, info.max_subevent, info.unicast.central.bn, ft,
		info.unicast.central.max_pdu, info.unicast.subinterval,
		info.unicast.central.latency);
	if (ft < 2U && bcast_source_iso_ms() > 10U) {
		LOG_WRN("Phone CIS FT %u: each CIS packet the 20 ms BIG blocks is lost - compare "
			"with 'bike isoint 10'", ft);
	}
}

static void st_enabled(struct bt_bap_stream *stream)
{
	/* ASE sink: phia server (hub) bao Receiver Start Ready */
	int err = bt_bap_stream_start(stream);

	if (err) {
		LOG_ERR("Start ASE %u failed (%d)", stream_idx(stream), err);
	}
}

static void st_started(struct bt_bap_stream *stream)
{
	struct ph_stream *s = CONTAINER_OF(stream, struct ph_stream, stream);

	if (stream->codec_cfg == NULL || parse_cfg(stream->codec_cfg, &s->cfg) != 0) {
		LOG_ERR("ASE %u: unsupported codec config at start", stream_idx(stream));
		return;
	}
	s->mask = (s->cfg.chan_cnt == 2) ? (BIKE_CH_L | BIKE_CH_R) : mono_mask(s->cfg.alloc);
	s->rx = 0;
	s->bad = 0;
	s->run = 0;
	s->max_run = 0;
	s->l_rx = 0;
	s->l_bad = 0;
	s->streaming = true;
	update_expected();
	LOG_INF("Phone streaming ASE %u: %u ch (loc 0x%x), %u B/frame -> broadcast %s",
		stream_idx(stream), s->cfg.chan_cnt, s->cfg.alloc, s->cfg.octets,
		s->mask == (BIKE_CH_L | BIKE_CH_R) ? "L+R" : (s->mask == BIKE_CH_L ? "L" : "R"));
	log_cis_params(stream);
	bcast_source_phone_started(s->cfg.octets);
}

static void st_end(struct bt_bap_stream *stream, const char *why, uint8_t reason)
{
	struct ph_stream *s = CONTAINER_OF(stream, struct ph_stream, stream);

	if (!s->streaming) {
		return;
	}
	s->streaming = false;
	update_expected();
	flush_pair();
	LOG_INF("ASE %u %s (reason 0x%02x, %u SDU, %u bad)", stream_idx(stream), why, reason,
		s->rx, s->bad);
	if (!phone_sink_streaming()) {
		bcast_source_phone_stopped();
	}
}

static void st_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	st_end(stream, "stopped", reason);
}

static void st_released(struct bt_bap_stream *stream)
{
	st_end(stream, "released", 0);
}

static void st_recv(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
		    struct net_buf *buf)
{
	struct ph_stream *s = CONTAINER_OF(stream, struct ph_stream, stream);
	const uint16_t oct = s->cfg.octets;
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0 &&
			   buf->len >= (uint32_t)oct * s->cfg.chan_cnt;
	const uint8_t *d = valid ? buf->data : NULL;

	if (!s->streaming) {
		return;
	}
	s->rx++;
	if (!valid) {
		s->bad++;
		s->run++;
		s->max_run = MAX(s->max_run, s->run);
	} else {
		s->run = 0;
	}

	if (s->cfg.chan_cnt == 2) {
		flush_pair();
		bcast_source_push_pair(d, d ? d + oct : NULL, oct);
	} else if (s->mask == (BIKE_CH_L | BIKE_CH_R) || expected == s->mask) {
		/* Mono, hoac chi mot ben dang chay: cung khung cho ca hai mu-tai */
		flush_pair();
		bcast_source_push_pair(d, d, oct);
	} else {
		put_side(s->mask, d, oct, info->ts);
	}
}

static struct bt_bap_stream_ops stream_ops = {
	.enabled = st_enabled,
	.started = st_started,
	.stopped = st_stopped,
	.released = st_released,
	.recv = st_recv,
};

/* ---------------- API ---------------- */

int phone_sink_register(void)
{
	static struct bt_bap_unicast_server_register_param param = {
		.snk_cnt = CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT,
		.src_cnt = 0,
	};
	const struct bt_pacs_register_param pacs_param = {
		.snk_pac = true,
		.snk_loc = true,
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
	}
	return err;
}

int phone_sink_init(void)
{
	int err;

	err = bt_bap_unicast_server_register_cb(&ascs_cb);
	if (err) {
		LOG_ERR("Unicast server callbacks failed (%d)", err);
		return err;
	}
	err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (err) {
		LOG_ERR("PACS capability register failed (%d)", err);
		return err;
	}
	(void)bt_pacs_set_location(BT_AUDIO_DIR_SINK, SINK_LOCATION);
	(void)bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SINK, SINK_CONTEXTS);
	(void)bt_pacs_set_available_contexts(BT_AUDIO_DIR_SINK, SINK_CONTEXTS);

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		bt_bap_stream_cb_register(&streams[i].stream, &stream_ops);
	}
	LOG_INF("Phone side: %u sink ASE, LC3 48 kHz / 10 ms / %u-%u B, 1-2 ch, no microphone",
		CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT, BIKE_OCTETS_MIN, BIKE_OCTETS_MAX);
	return 0;
}

bool phone_sink_streaming(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		if (streams[i].streaming) {
			return true;
		}
	}
	return false;
}

/* Thread main (heartbeat 10 s) */
void phone_sink_log_status(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		struct ph_stream *s = &streams[i];
		const uint32_t rx = s->rx;
		const uint32_t bad = s->bad;

		if (!s->streaming) {
			continue;
		}
		LOG_INF("phone CIS ASE %u | 10 s: rx %u, lost %u, longest loss %u SDU (%u ms)",
			(unsigned)i, rx - s->l_rx, bad - s->l_bad, s->max_run,
			s->max_run * (BIKE_FRAME_US / 1000U));
		s->l_rx = rx;
		s->l_bad = bad;
		s->max_run = 0;
	}
}

void phone_sink_print(const struct shell *sh)
{
	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		const struct ph_stream *s = &streams[i];

		if (s->stream.conn == NULL) {
			shell_print(sh, "ASE %u     : free", (unsigned)i);
		} else {
			shell_print(sh, "ASE %u     : %s, %u ch loc 0x%x, %u B/frame, rx %u, bad %u",
				    (unsigned)i, s->streaming ? "STREAMING" : "configured",
				    s->cfg.chan_cnt, s->cfg.alloc, s->cfg.octets, s->rx, s->bad);
		}
	}
}
