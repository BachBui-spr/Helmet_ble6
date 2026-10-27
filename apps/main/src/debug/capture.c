/*
 * Helmet - ghi lai luong LE Audio nhan duoc va gui ve PC qua J-Link (SEGGER RTT kenh 1).
 *
 * Moi SDU LC3 nhan tu dien thoai duoc chep nguyen ven (chua giai ma) vao kenh RTT 1,
 * kem cau hinh codec, so thu tu ISO va timestamp. Script scripts/helmet_audio.py tren PC
 * doc kenh nay, giai ma LC3 roi phat ra loa may tinh / luu WAV.
 *
 * - Kenh RTT 0 van danh cho log + shell; kenh 1 chi chua du lieu nhi phan.
 * - Che do NO_BLOCK_SKIP: bo dem day (PC doc khong kip / khong cam J-Link) thi bo ca ban ghi,
 *   khong bao gio lam cham luong nhan. PC thay so thu tu ban ghi nhay coc -> bu bang PLC.
 * - Mac dinh TAT; bat bang "helmet capture on" (script tu gui lenh nay).
 * - Chi co khi build voi EXTRA=-DCONFIG_HELMET_CAPTURE=y (apps/main/Kconfig): bo dem RTT 16 KB RAM
 *   mac dinh nhuong cho che do Dinh vi (them 1 ket noi + Channel Sounding). Khong build thi cac
 *   ham duoi day la rong va "helmet capture" bao cach bat.
 * - Bang thong lon nhat: 48 kHz stereo, 2 x 155 B / 10 ms ~ 33 KB/s.
 *
 * Dinh dang ban ghi (little-endian):
 *   header 8 B : A5 5A | type u8 | stream u8 | len u16 (do dai payload) | rec_seq u16
 *   CFG  (1)   : freq_hz u32 | frame_dur_us u32 | octets_per_frame u16 | chan_cnt u8 | 0 |
 *                chan_alloc u32
 *   SDU  (2)   : iso_flags u8 (BT_ISO_FLAGS_*) | 0 | iso_seq u16 | ts u32 | LC3 (len - 8 B)
 *   STOP (3)   : khong co payload
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <SEGGER_RTT.h>

#include <me54/services/audio_stream.h>

#include "debug/capture.h"
#include "features/phone/le_audio.h"

LOG_MODULE_REGISTER(capture, LOG_LEVEL_INF);

#if defined(CONFIG_HELMET_CAPTURE)

#define CAP_RTT_CHANNEL  1
/* 16 KB ~ 0.5 s o 33 KB/s: J-Link doc cham mot nhip van kip */
#define CAP_RTT_BUF_SIZE CONFIG_HELMET_CAPTURE_BUF
#define CFG_RESEND_SDU   100   /* gui lai cau hinh ~1 s/lan: PC mo giua chung van bat kip */

#define REC_SYNC0   0xA5
#define REC_SYNC1   0x5A
#define REC_HDR_LEN 8
#define REC_CFG_LEN 16
#define REC_SDU_HDR 8

BUILD_ASSERT(CONFIG_SEGGER_RTT_MAX_NUM_UP_BUFFERS > CAP_RTT_CHANNEL,
	     "can them RTT up-buffer cho kenh am thanh");

enum rec_type {
	REC_CFG = 1,
	REC_SDU = 2,
	REC_STOP = 3,
};

struct cap_stream {
	bool active;
	struct audio_stream_cfg cfg;
	uint32_t sdu_since_cfg;
};

static uint8_t rtt_buf[CAP_RTT_BUF_SIZE];

static atomic_t enabled;
static atomic_t resend_cfg;

/* Cac ham ben duoi co the duoc goi tu nhieu thread Bluetooth -> khoa chung */
K_MUTEX_DEFINE(cap_lock);
static struct cap_stream cs[HELMET_MAX_STREAMS];
static uint8_t rec[REC_HDR_LEN + REC_SDU_HDR + AUDIO_STREAM_MAX_SDU];
static uint16_t rec_seq;
static uint32_t rec_sent;
static uint32_t rec_dropped;

static void send_rec(uint8_t type, uint8_t idx, uint16_t len)
{
	rec[0] = REC_SYNC0;
	rec[1] = REC_SYNC1;
	rec[2] = type;
	rec[3] = idx;
	sys_put_le16(len, &rec[4]);
	sys_put_le16(rec_seq++, &rec[6]);

	/* SKIP: hoac ghi tron ca ban ghi, hoac khong ghi gi (tra ve 0) */
	if (SEGGER_RTT_Write(CAP_RTT_CHANNEL, rec, REC_HDR_LEN + len) == 0) {
		rec_dropped++;
	} else {
		rec_sent++;
	}
}

static void send_cfg(uint8_t idx)
{
	const struct audio_stream_cfg *cfg = &cs[idx].cfg;
	uint8_t *p = &rec[REC_HDR_LEN];

	sys_put_le32(cfg->freq_hz, &p[0]);
	sys_put_le32(cfg->frame_dur_us, &p[4]);
	sys_put_le16(cfg->octets_per_frame, &p[8]);
	p[10] = cfg->chan_cnt;
	p[11] = 0;
	sys_put_le32(cfg->chan_alloc, &p[12]);

	send_rec(REC_CFG, idx, REC_CFG_LEN);
	cs[idx].sdu_since_cfg = 0;
}

int capture_init(void)
{
	int err = SEGGER_RTT_ConfigUpBuffer(CAP_RTT_CHANNEL, "HelmetAudio", rtt_buf,
					    sizeof(rtt_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);

	if (err < 0) {
		LOG_ERR("RTT channel %u config failed (%d)", CAP_RTT_CHANNEL, err);
		return -EIO;
	}
	return 0;
}

void capture_set_enabled(bool on)
{
	if (on) {
		atomic_set(&resend_cfg, 1);
	}
	atomic_set(&enabled, on ? 1 : 0);
	LOG_INF("Audio capture %s (RTT channel %u)", on ? "ON" : "OFF", CAP_RTT_CHANNEL);
}

void capture_stream_start(uint8_t idx, const struct audio_stream_cfg *cfg)
{
	if (idx >= ARRAY_SIZE(cs)) {
		return;
	}

	k_mutex_lock(&cap_lock, K_FOREVER);
	cs[idx].cfg = *cfg;
	cs[idx].active = true;
	if (atomic_get(&enabled)) {
		send_cfg(idx);
	}
	k_mutex_unlock(&cap_lock);
}

void capture_stream_stop(uint8_t idx)
{
	if (idx >= ARRAY_SIZE(cs)) {
		return;
	}

	k_mutex_lock(&cap_lock, K_FOREVER);
	if (cs[idx].active) {
		cs[idx].active = false;
		if (atomic_get(&enabled)) {
			send_rec(REC_STOP, idx, 0);
		}
	}
	k_mutex_unlock(&cap_lock);
}

void capture_sdu(uint8_t idx, uint8_t iso_flags, uint16_t iso_seq, uint32_t ts,
		 const uint8_t *data, uint16_t len)
{
	uint8_t *p = &rec[REC_HDR_LEN];

	if (!atomic_get(&enabled) || idx >= ARRAY_SIZE(cs)) {
		return;
	}
	if (len > AUDIO_STREAM_MAX_SDU) {
		len = 0;
	}

	k_mutex_lock(&cap_lock, K_FOREVER);
	if (!cs[idx].active) {
		k_mutex_unlock(&cap_lock);
		return;
	}

	if (atomic_clear(&resend_cfg)) {
		for (uint8_t i = 0; i < ARRAY_SIZE(cs); i++) {
			if (cs[i].active) {
				send_cfg(i);
			}
		}
	} else if (cs[idx].sdu_since_cfg >= CFG_RESEND_SDU) {
		send_cfg(idx);
	}

	p[0] = iso_flags;
	p[1] = 0;
	sys_put_le16(iso_seq, &p[2]);
	sys_put_le32(ts, &p[4]);
	if (len > 0) {
		memcpy(&p[REC_SDU_HDR], data, len);
	}
	send_rec(REC_SDU, idx, REC_SDU_HDR + len);
	cs[idx].sdu_since_cfg++;
	k_mutex_unlock(&cap_lock);
}

void capture_print_status(const struct shell *sh)
{
	shell_print(sh, "Capture   : %s, RTT channel %u, records sent %u, dropped %u (buffer full), "
		    "%u B waiting",
		    atomic_get(&enabled) ? "ON" : "off", CAP_RTT_CHANNEL, rec_sent, rec_dropped,
		    SEGGER_RTT_GetBytesInBuffer(CAP_RTT_CHANNEL));
}

#else /* !CONFIG_HELMET_CAPTURE: khong build */

int capture_init(void)
{
	return -ENOTSUP;
}

void capture_set_enabled(bool on)
{
	if (on) {
		LOG_WRN("Audio capture not built in - rebuild with EXTRA=-DCONFIG_HELMET_CAPTURE=y "
			"(uses 16 KB RAM)");
	}
}

void capture_stream_start(uint8_t idx, const struct audio_stream_cfg *cfg)
{
	ARG_UNUSED(idx);
	ARG_UNUSED(cfg);
}

void capture_stream_stop(uint8_t idx)
{
	ARG_UNUSED(idx);
}

void capture_sdu(uint8_t idx, uint8_t iso_flags, uint16_t iso_seq, uint32_t ts,
		 const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(idx);
	ARG_UNUSED(iso_flags);
	ARG_UNUSED(iso_seq);
	ARG_UNUSED(ts);
	ARG_UNUSED(data);
	ARG_UNUSED(len);
}

void capture_print_status(const struct shell *sh)
{
	shell_print(sh, "Capture   : not built in (make APP=apps/main "
		    "EXTRA=-DCONFIG_HELMET_CAPTURE=y build; uses 16 KB RAM)");
}

#endif /* CONFIG_HELMET_CAPTURE */
