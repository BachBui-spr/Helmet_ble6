/*
 * Lop DICH VU: duong xuat am thanh cua mu (giai ma LC3 -> bu troi -> am bao -> lay mau lai ->
 * I2S). Toi da AUDIO_OUT_STREAMS luong cung luc (vd ASE trai + phai, 2 BIS, 1 CIS intercom).
 */

#ifndef ME54_SERVICES_AUDIO_OUT_H_
#define ME54_SERVICES_AUDIO_OUT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <me54/core/prompts.h>
#include <me54/services/audio_stream.h>

#define AUDIO_OUT_STREAMS CONFIG_ME54_SVC_AUDIO_OUT_STREAMS

struct audio_out_stats {
	uint32_t sdu_rx;       /* SDU nhan duoc */
	uint32_t sdu_bad;      /* SDU loi/mat (da bu bang PLC) */
	uint32_t plc;          /* so kenh-khung phai noi suy */
	uint32_t dec_err;      /* loi giai ma */
	uint32_t len_err;      /* SDU sai do dai */
	uint32_t q_full;       /* SDU bi bo vi hang doi day */
	uint32_t slip_drop;    /* mau bi bo de bu troi clock dien thoai */
	uint32_t slip_insert;  /* mau duoc chen them */
	uint32_t underrun;     /* khung phai phat im lang vi het du lieu */
	uint32_t ring_drop;    /* khung bi bo vi vong PCM day */
	uint32_t i2s_err;      /* loi I2S (da tu khoi phuc) */
	uint32_t out_rate;     /* tan so danh dinh dang phat */
	uint32_t i2s_rate_mhz; /* tan so I2S that (mHz) */
	uint16_t frame_samples;
	uint16_t ring_level;   /* mau stereo dang cho trong vong PCM */
	uint8_t active_mask;   /* BIT(0) = kenh trai, BIT(1) = kenh phai dang nhan */
	uint8_t queued_blocks; /* so khoi PCM dang cho trong I2S */
	bool i2s_running;
	bool prompt_active;
};

int audio_out_init(void);
void audio_out_stream_start(uint8_t idx, const struct audio_stream_cfg *cfg);
void audio_out_stream_stop(uint8_t idx);
/* Goi tu BT RX thread (thread duy nhat goi ham nay): chi copy du lieu, khong giai ma */
void audio_out_stream_data(uint8_t idx, bool valid, uint32_t ts, const uint8_t *data,
			   uint16_t len);
void audio_out_set_volume(uint8_t volume, bool mute);
void audio_out_prompt(enum prompt_id p);
void audio_out_prompt_stop(void);
bool audio_out_prompt_busy(void);
void audio_out_shutdown(void);
void audio_out_get_stats(struct audio_out_stats *stats);
void audio_out_print_perf(const struct shell *sh);

/* Tham chieu cho bo khu vong, mien 16 kHz.
 * anchor: mau tham chieu thu ref_pos bat dau phat ra loa tai thoi diem t_cyc (k_cycle_get_32).
 * gen doi moi khi dau ra khoi dong lai -> ben doc phai tinh lai do lech.
 */
bool audio_out_ref_anchor(uint32_t *ref_pos, uint32_t *t_cyc, uint32_t *gen);
/* Tra ve so mau that co (cac mau thieu duoc ghi 0) */
int audio_out_ref_read(int16_t *dst, uint16_t n, uint32_t pos);

#endif /* ME54_SERVICES_AUDIO_OUT_H_ */
