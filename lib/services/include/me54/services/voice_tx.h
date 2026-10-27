/*
 * Lop DICH VU: chieu len cua cuoc goi / intercom: micro -> DSP -> bu troi clock -> LC3 -> SDU.
 */

#ifndef ME54_SERVICES_VOICE_TX_H_
#define ME54_SERVICES_VOICE_TX_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/shell/shell.h>

#include <me54/hal/mic_in.h>
#include <me54/services/audio_stream.h>

/* Loi xu ly giong noi luon chay 16 kHz, khung 10 ms (nhip micro) */
#define VOICE_RATE  MIC_IN_RATE
#define VOICE_FRAME MIC_IN_FRAME

struct voice_tx_stats {
	uint32_t sessions;
	uint32_t mic_blocks;
	uint32_t mic_err;
	uint32_t mic_restart;  /* micro duoc khoi dong lai sau loi doc lien tiep */
	uint32_t sdu_sent;
	uint32_t sdu_underrun; /* het SDU khi controller can -> gui im lang */
	uint32_t send_err;
	uint32_t pool_empty;
	uint32_t q_drop;       /* SDU bi bo vi hang day */
	uint32_t slip_drop;    /* mau bo de bu troi clock */
	uint32_t slip_insert;  /* mau chen */
	uint32_t ref_missing;  /* khoi khong co tham chieu */
	int16_t mic_dbfs_x10;
	int16_t out_dbfs_x10;
	int16_t erle_db_x10;
	uint32_t out_rate;
	bool active;
	bool muted;
	bool mic_ok;
};

/* Lop tren dang ky noi nhan bao "micro khong dung duoc" (goi tu thread giong noi) */
void voice_set_mic_unavailable_cb(void (*cb)(bool unavailable));
void voice_start(struct bt_bap_stream *stream, const struct audio_stream_cfg *cfg);
void voice_stop(void);
void voice_on_sent(struct bt_bap_stream *stream);
void voice_set_mute(bool mute);
/* Thu nghiem: thay giong noi bang am sin hz (0 = tat), sau DSP, khong qua khu vong/giam on */
void voice_set_tone(uint16_t hz);
bool voice_is_muted(void);
void voice_set_dsp(bool aec, bool ns, bool agc);
void voice_get_dsp(bool *aec, bool *ns, bool *agc);
void voice_get_stats(struct voice_tx_stats *st);
void voice_print_status(const struct shell *sh);
void voice_print_perf(const struct shell *sh);

#endif /* ME54_SERVICES_VOICE_TX_H_ */
