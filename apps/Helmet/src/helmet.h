/*
 * Helmet - khai bao dung chung giua cac module.
 *
 *   le_audio.c   : BAP Unicast Server (nhan luong LC3 tu dien thoai qua CIS)
 *   media_ctrl.c : VCP Volume Renderer (am luong) + MCP client (play/pause)
 *   audio_out.c  : giai ma LC3 -> PCM -> I2S20 -> 2 x MAX98357A
 *   capture.c    : chep SDU LC3 nhan duoc ra RTT kenh 1 de nghe lai tren PC (J-Link)
 *   main.c       : Bluetooth, quang ba, ket noi, nut nhan, shell, heartbeat
 */

#ifndef HELMET_H_
#define HELMET_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

/* Ngu canh am thanh Helmet nhan duoc (chua co micro -> khong nhan cuoc goi) */
#define HELMET_SINK_CONTEXTS                                                                       \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_MEDIA |                         \
	 BT_AUDIO_CONTEXT_TYPE_GAME | BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL |                        \
	 BT_AUDIO_CONTEXT_TYPE_SOUND_EFFECTS | BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS |               \
	 BT_AUDIO_CONTEXT_TYPE_ALERTS)

/* Vi tri loa: Helmet co ca loa trai va phai */
#define HELMET_SINK_LOCATION (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)

#define HELMET_MAX_STREAMS CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT

/* SDU lon nhat: 2 kenh x 155 octet/khung x 1 khung/SDU */
#define HELMET_MAX_SDU_LEN 320

/* Cau hinh codec cua mot luong (ASE sink) da duoc dien thoai chon */
struct helmet_stream_cfg {
	uint32_t freq_hz;
	uint32_t frame_dur_us;
	uint16_t octets_per_frame;
	uint8_t chan_cnt;
	uint32_t chan_alloc; /* enum bt_audio_location, 0 = mono */
};

/* Thong ke duong am thanh (de giam sat qua shell/heartbeat) */
struct helmet_audio_stats {
	uint32_t sdu_rx;       /* SDU nhan duoc */
	uint32_t sdu_bad;      /* SDU loi/mat (da bu bang PLC) */
	uint32_t plc;          /* so khung LC3 phai noi suy (Packet Loss Concealment) */
	uint32_t dec_err;      /* loi giai ma */
	uint32_t len_err;      /* SDU sai do dai */
	uint32_t q_full;       /* SDU bi bo vi hang doi day */
	uint32_t drift_drop;   /* khoi 1 khung bi bo de bu lech clock (bo dem qua day) */
	uint32_t drift_repeat; /* khoi bi lap lai de bu lech clock (bo dem sap can) */
	uint32_t i2s_err;      /* loi/underrun I2S (da tu khoi phuc) */
	uint32_t out_rate;     /* tan so lay mau dang xuat ra I2S */
	uint16_t frame_samples;
	uint8_t active_mask;   /* BIT(0) = kenh trai, BIT(1) = kenh phai dang nhan */
	uint8_t queued_blocks; /* so khoi PCM dang cho trong I2S */
	bool i2s_running;
};

/* ---- audio_out.c ---- */
int audio_out_init(void);
void audio_out_stream_start(uint8_t idx, const struct helmet_stream_cfg *cfg);
void audio_out_stream_stop(uint8_t idx);
void audio_out_stream_data(uint8_t idx, bool valid, uint32_t ts, const uint8_t *data,
			   uint16_t len);
void audio_out_set_volume(uint8_t volume, bool mute);
void audio_out_get_stats(struct helmet_audio_stats *stats);

/* SDU nhan duoc phan loai theo co ISO cua controller */
struct helmet_rx_flags {
	uint32_t ok;    /* du lieu tot */
	uint32_t crc;   /* BT_ISO_FLAGS_ERROR: nhan duoc nhung co the hong */
	uint32_t lost;  /* BT_ISO_FLAGS_LOST: khong nhan duoc gi */
	uint32_t empty; /* hop le nhung do dai 0 */
};

/* ---- le_audio.c ---- */
int le_audio_init(void);
bool le_audio_is_streaming(void);
void le_audio_print_status(const struct shell *sh);
const char *le_audio_phy_str(uint8_t phy);
void le_audio_set_qos_pref(uint8_t phy, uint8_t rtn, uint16_t latency_ms);
void le_audio_get_rx_flags(struct helmet_rx_flags *out);
void le_audio_print_link_quality(const struct shell *sh);

/* ---- media_ctrl.c ---- */
int media_ctrl_init(void);
void media_ctrl_connected(struct bt_conn *conn);
void media_ctrl_disconnected(void);
int media_ctrl_play_pause(void);
int media_ctrl_play(void);
int media_ctrl_pause(void);
int media_ctrl_volume_up(void);
int media_ctrl_volume_down(void);
int media_ctrl_set_volume(uint8_t volume);
void media_ctrl_print_status(const struct shell *sh);

/* ---- capture.c (gui SDU LC3 nhan duoc ve PC qua RTT kenh 1) ---- */
int capture_init(void);
void capture_set_enabled(bool on);
void capture_stream_start(uint8_t idx, const struct helmet_stream_cfg *cfg);
void capture_stream_stop(uint8_t idx);
void capture_sdu(uint8_t idx, uint8_t iso_flags, uint16_t iso_seq, uint32_t ts,
		 const uint8_t *data, uint16_t len);
void capture_print_status(const struct shell *sh);

#endif /* HELMET_H_ */
