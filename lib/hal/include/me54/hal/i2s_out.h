/*
 * Lop HAL: I2S20 -> 2 x MAX98357A, 16 bit stereo, controller cap BCLK / LRCLK.
 *
 * Kho khoi (mem slab) nam o day: CONFIG_ME54_HAL_I2S_OUT_BLOCK_BYTES x _BLOCK_COUNT. Nhip nap,
 * so khoi muc tieu, luc bat dau / dung la viec cua dich vu (services/audio_out.c,
 * services/prompt_player.c).
 *
 * nRF54L15: I2S chi chia nguyen tu 32 MHz nen khong ra dung 48/32/24/16 kHz (vd 48 kHz ->
 * 47 619,05 Hz). i2s_out_actual_rate() tinh tan so that bang chinh nrfx_i2s_prescalers_calc()
 * ma driver dung, kem ti le lay mau lai dang phan so toi gian.
 */

#ifndef ME54_HAL_I2S_OUT_H_
#define ME54_HAL_I2S_OUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define I2S_OUT_BLOCK_BYTES CONFIG_ME54_HAL_I2S_OUT_BLOCK_BYTES
#define I2S_OUT_BLOCK_COUNT CONFIG_ME54_HAL_I2S_OUT_BLOCK_COUNT

struct i2s_out_rate {
	uint32_t num;        /* mau vao / mau ra = num / den (1 / 1: khong lay mau lai) */
	uint32_t den;
	uint32_t actual_mhz; /* tan so that, mHz */
	uint32_t mck_div;    /* MCK = 32 MHz / mck_div */
	uint32_t ratio;      /* MCK / LRCLK */
};

int i2s_out_init(void); /* -ENODEV khi I2S20 chua san sang */
/* Cau hinh 16 bit stereo, khoi block_samples mau stereo; timeout_ms cho i2s_write */
int i2s_out_configure(uint32_t rate, uint16_t block_samples, int32_t timeout_ms);
bool i2s_out_configured(void);
/* Dung ngay (DROP; loi thi PREPARE de ve READY) va cho driver tra het khoi (toi da 100 ms) */
void i2s_out_halt(void);
int i2s_out_start(void);
int i2s_out_drain(void);
int i2s_out_drop(void);
int i2s_out_alloc(void **blk, k_timeout_t timeout);
void i2s_out_free(void *blk);
int i2s_out_write(void *blk, size_t size);
uint8_t i2s_out_blocks_in_use(void);
/* Cho toi khi driver tra het khoi: toi da 'tries' lan, moi lan step_ms */
void i2s_out_wait_released(uint32_t tries, uint32_t step_ms);

/* Tan so that cho 'rate'. Loi: -EIO (khong tinh duoc prescaler), -ENOTSUP (cau hinh clock la),
 * -ERANGE (lech > 5 %); khi loi out->num = out->den = 1, out->actual_mhz = rate x 1000.
 */
int i2s_out_actual_rate(uint32_t rate, struct i2s_out_rate *out);

#endif /* ME54_HAL_I2S_OUT_H_ */
