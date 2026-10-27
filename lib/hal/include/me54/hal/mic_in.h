/*
 * Lop HAL: 2 micro PDM (IM69D130) tren PDM20, 16 kHz, 16 bit, stereo, khoi 10 ms.
 */

#ifndef ME54_HAL_MIC_IN_H_
#define ME54_HAL_MIC_IN_H_

#include <stdbool.h>
#include <stdint.h>

#define MIC_IN_RATE  16000
#define MIC_IN_FRAME 160 /* mau moi kenh moi khoi 10 ms */

int mic_in_init(void);
bool mic_in_ready(void);
int mic_in_start(uint32_t *t0_cyc);
/* Dung + chay lai (driver tu dung PDM khi hang doi day). t0_cyc: thoi diem bat dau moi */
int mic_in_restart(uint32_t *t0_cyc);
/* Doc 1 khoi 10 ms: 160 mau x 2 kenh (L, R xen ke) */
int mic_in_read(int16_t **block, int32_t timeout_ms);
void mic_in_free(int16_t *block);
void mic_in_stop(void);

#endif /* ME54_HAL_MIC_IN_H_ */
