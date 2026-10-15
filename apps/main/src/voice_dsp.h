/*
 * Helmet - xu ly giong noi (C thuan, khong dung API Zephyr -> bien dich duoc tren PC de chay
 * thu tren du lieu ghi that).
 *
 * Moi khung 10 ms o 16 kHz (160 mau):
 *   gop 2 micro -> loc thong cao 100 Hz -> khu vong NLMS 256 he so (16 ms) -> NLP
 *   -> giam on pho (cua so 20 ms, FFT 512) -> AGC -> (tat micro)
 */

#ifndef VOICE_DSP_H_
#define VOICE_DSP_H_

#include <stdbool.h>
#include <stdint.h>

#define VDSP_FS   16000
#define VDSP_N    160
#define VDSP_TAPS 256

struct vdsp_stats {
	float mic_dbfs;  /* muc micro (sau loc thong cao) */
	float out_dbfs;  /* muc gui di */
	float erle_db;   /* do giam tieng vong cua bo loc thich nghi */
	uint32_t dt_blocks;   /* khoi bi coi la "ca hai cung noi" */
	uint32_t far_blocks;  /* khoi co tieng nguoi ben kia */
	uint32_t resets;      /* so lan bo loc phan ky phai lam lai */
};

void vdsp_init(void);
void vdsp_reset(void);
void vdsp_aec_reset(void);
void vdsp_set(bool aec, bool ns, bool agc);
void vdsp_get_flags(bool *aec, bool *ns, bool *agc);

/* mic_lr: 160 mau x 2 kenh xen ke (L, R); ref: 160 mau tham chieu (mau da ra loa) hoac NULL.
 * out: 160 mau gui di.
 */
void vdsp_process(const int16_t *mic_lr, const int16_t *ref, int16_t *out);
void vdsp_get_stats(struct vdsp_stats *st);

#endif /* VOICE_DSP_H_ */
