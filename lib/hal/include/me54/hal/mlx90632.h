/*
 * Lop HAL - MLX90632 (cam bien nhiet hong ngoai, I2C 0x3A): thanh ghi + I/O.
 * Thanh ghi theo thu vien chinh thuc cua Melexis (mlx90632-library, Apache-2.0).
 * Kieu du lieu calib / raw va cong thuc: <me54/core/mlx90632_calc.h>.
 */

#ifndef ME54_HAL_MLX90632_H_
#define ME54_HAL_MLX90632_H_

#include <stdint.h>

#include <zephyr/device.h>

#include <me54/core/mlx90632_calc.h>

#define MLX90632_ADDR 0x3A

/* EEPROM (32 bit: word thap o dia chi, word cao o dia chi + 1) */
#define MLX90632_EE_VERSION 0x240B
#define MLX90632_EE_P_R     0x240C
#define MLX90632_EE_P_G     0x240E
#define MLX90632_EE_P_T     0x2410
#define MLX90632_EE_P_O     0x2412
#define MLX90632_EE_Ea      0x2424
#define MLX90632_EE_Eb      0x2426
#define MLX90632_EE_Fa      0x2428
#define MLX90632_EE_Fb      0x242A
#define MLX90632_EE_Ga      0x242C
#define MLX90632_EE_Gb      0x242E /* 16 bit */
#define MLX90632_EE_Ka      0x242F /* 16 bit */
#define MLX90632_EE_Ha      0x2481 /* 16 bit */
#define MLX90632_EE_Hb      0x2482 /* 16 bit */
#define MLX90632_EE_MEDICAL_MEAS1 0x24E1
#define MLX90632_EE_MEDICAL_MEAS2 0x24E2

/* Thanh ghi */
#define MLX90632_REG_CTRL   0x3001
#define MLX90632_REG_STATUS 0x3FFF
#define MLX90632_RAM_1(n)   (0x4000 + 3 * (n))
#define MLX90632_RAM_2(n)   (0x4000 + 3 * (n) + 1)
#define MLX90632_RAM_3(n)   (0x4000 + 3 * (n) + 2)

/* REG_STATUS */
#define MLX90632_STAT_BUSY     (1U << 10)
#define MLX90632_STAT_EE_BUSY  (1U << 9)
#define MLX90632_STAT_DATA_RDY (1U << 0)
#define MLX90632_STAT_CYCLE_POS(s) (((s) >> 2) & 0x1F)

/* REG_CTRL */
#define MLX90632_CTRL_SOC        (1U << 3)  /* bat dau do 1 lan (step) */
#define MLX90632_CTRL_SOB        (1U << 11) /* bat dau do 1 luot day du (burst) */
#define MLX90632_CTRL_PWR_MASK   (0x3U << 1)
#define MLX90632_CTRL_PWR_HALT   (0U << 1)
#define MLX90632_CTRL_PWR_SLEEP_STEP (1U << 1)
#define MLX90632_CTRL_PWR_STEP   (2U << 1)
#define MLX90632_CTRL_PWR_CONT   (3U << 1)
#define MLX90632_CTRL_MTYP_MASK  (0x1FU << 4)
#define MLX90632_MTYP_MEDICAL    0x00

#define MLX90632_DSPV5        0x05
#define MLX90632_MEAS_MAX_MS  2000 /* tan so do 0.5 Hz */

int mlx90632_probe(const struct device *i2c, uint16_t *version);
int mlx90632_read_cal(const struct device *i2c, struct mlx90632_cal *cal);
/* Che do sleeping step + loai do medical; tra ve thoi gian 1 luot do (ms) */
int mlx90632_config(const struct device *i2c, uint32_t *dataset_ms);
int mlx90632_measure(const struct device *i2c, uint32_t dataset_ms, struct mlx90632_raw *raw);
int mlx90632_halt(const struct device *i2c);

#endif /* ME54_HAL_MLX90632_H_ */
