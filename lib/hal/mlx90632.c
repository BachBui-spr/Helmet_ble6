/*
 * Lop HAL - MLX90632 (cam bien nhiet hong ngoai, I2C 0x3A): doc / ghi thanh ghi, EEPROM, mot
 * luot do. Cong thuc tinh nhiet o lib/core/mlx90632_calc.c.
 *
 * Trinh tu 1 lan do (che do sleeping step, giong mlx90632_start_measurement_burst):
 *   xoa co DATA_RDY -> dat SOB -> ngu het thoi gian 1 luot do (2 kenh) -> cho het BUSY
 *   -> doc RAM theo vi tri kenh 2 (kenh moi = 2, kenh cu = 1).
 */

#include <errno.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include <me54/hal/mlx90632.h>

/* Dia chi thanh ghi 16 bit va du lieu 16 bit, deu big-endian */
static int rd16(const struct device *i2c, uint16_t reg, uint16_t *val)
{
	const uint8_t addr[2] = {reg >> 8, reg & 0xFF};
	uint8_t data[2];
	int err = i2c_write_read(i2c, MLX90632_ADDR, addr, sizeof(addr), data, sizeof(data));

	if (err == 0) {
		*val = ((uint16_t)data[0] << 8) | data[1];
	}
	return err;
}

static int wr16(const struct device *i2c, uint16_t reg, uint16_t val)
{
	const uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};

	return i2c_write(i2c, buf, sizeof(buf), MLX90632_ADDR);
}

static int rd32(const struct device *i2c, uint16_t reg, int32_t *val)
{
	uint16_t lo;
	uint16_t hi;
	int err = rd16(i2c, reg, &lo);

	if (err == 0) {
		err = rd16(i2c, reg + 1, &hi);
	}
	if (err == 0) {
		*val = (int32_t)(((uint32_t)hi << 16) | lo);
	}
	return err;
}

static int rds16(const struct device *i2c, uint16_t reg, int16_t *val)
{
	uint16_t v;
	int err = rd16(i2c, reg, &v);

	if (err == 0) {
		*val = (int16_t)v;
	}
	return err;
}

int mlx90632_probe(const struct device *i2c, uint16_t *version)
{
	return rd16(i2c, MLX90632_EE_VERSION, version);
}

int mlx90632_read_cal(const struct device *i2c, struct mlx90632_cal *c)
{
	int err = 0;

	err |= rd32(i2c, MLX90632_EE_P_R, &c->P_R);
	err |= rd32(i2c, MLX90632_EE_P_G, &c->P_G);
	err |= rd32(i2c, MLX90632_EE_P_T, &c->P_T);
	err |= rd32(i2c, MLX90632_EE_P_O, &c->P_O);
	err |= rd32(i2c, MLX90632_EE_Ea, &c->Ea);
	err |= rd32(i2c, MLX90632_EE_Eb, &c->Eb);
	err |= rd32(i2c, MLX90632_EE_Fa, &c->Fa);
	err |= rd32(i2c, MLX90632_EE_Fb, &c->Fb);
	err |= rd32(i2c, MLX90632_EE_Ga, &c->Ga);
	err |= rds16(i2c, MLX90632_EE_Gb, &c->Gb);
	err |= rds16(i2c, MLX90632_EE_Ka, &c->Ka);
	err |= rds16(i2c, MLX90632_EE_Ha, &c->Ha);
	err |= rds16(i2c, MLX90632_EE_Hb, &c->Hb);

	return err ? -EIO : 0;
}

static uint32_t meas_ms(uint16_t ee)
{
	return MLX90632_MEAS_MAX_MS >> ((ee >> 8) & 0x7);
}

int mlx90632_config(const struct device *i2c, uint32_t *dataset_ms)
{
	uint16_t ctrl;
	uint16_t m1 = 0;
	uint16_t m2 = 0;
	int err;

	err = rd16(i2c, MLX90632_REG_CTRL, &ctrl);
	if (err) {
		return err;
	}
	ctrl &= ~(MLX90632_CTRL_PWR_MASK | MLX90632_CTRL_MTYP_MASK | MLX90632_CTRL_SOB |
		  MLX90632_CTRL_SOC);
	ctrl |= (MLX90632_MTYP_MEDICAL << 4) | MLX90632_CTRL_PWR_SLEEP_STEP;
	err = wr16(i2c, MLX90632_REG_CTRL, ctrl);
	if (err) {
		return err;
	}

	/* Thoi gian 1 luot do = tong thoi gian 2 kenh (theo tan so do trong EEPROM) */
	if (rd16(i2c, MLX90632_EE_MEDICAL_MEAS1, &m1) == 0 &&
	    rd16(i2c, MLX90632_EE_MEDICAL_MEAS2, &m2) == 0) {
		*dataset_ms = meas_ms(m1) + meas_ms(m2);
	} else {
		*dataset_ms = 1000;
	}
	return 0;
}

int mlx90632_measure(const struct device *i2c, uint32_t dataset_ms, struct mlx90632_raw *r)
{
	uint16_t status;
	uint16_t ctrl;
	int16_t a;
	int16_t b;
	int err;

	/* Xoa co du lieu moi */
	err = rd16(i2c, MLX90632_REG_STATUS, &status);
	if (err == 0) {
		err = wr16(i2c, MLX90632_REG_STATUS, status & ~MLX90632_STAT_DATA_RDY);
	}
	/* Bat dau 1 luot do */
	if (err == 0) {
		err = rd16(i2c, MLX90632_REG_CTRL, &ctrl);
	}
	if (err == 0) {
		err = wr16(i2c, MLX90632_REG_CTRL, ctrl | MLX90632_CTRL_SOB);
	}
	if (err) {
		return err;
	}

	k_msleep(dataset_ms);

	for (int i = 0; i < 100; i++) {
		err = rd16(i2c, MLX90632_REG_STATUS, &status);
		if (err) {
			return err;
		}
		if (!(status & MLX90632_STAT_BUSY)) {
			break;
		}
		if (i == 99) {
			return -ETIMEDOUT;
		}
		k_msleep(10);
	}

	/* Luot do day du: kenh moi = 2, kenh cu = 1 */
	r->cycle_pos = 2;
	err |= rds16(i2c, MLX90632_RAM_3(1), &r->ambient_new);
	err |= rds16(i2c, MLX90632_RAM_3(2), &r->ambient_old);
	err |= rds16(i2c, MLX90632_RAM_2(2), &a);
	err |= rds16(i2c, MLX90632_RAM_1(2), &b);
	r->object_new = (int16_t)((a + b) / 2);
	err |= rds16(i2c, MLX90632_RAM_2(1), &a);
	err |= rds16(i2c, MLX90632_RAM_1(1), &b);
	r->object_old = (int16_t)((a + b) / 2);

	return err ? -EIO : 0;
}

int mlx90632_halt(const struct device *i2c)
{
	uint16_t ctrl;
	int err = rd16(i2c, MLX90632_REG_CTRL, &ctrl);

	if (err == 0) {
		ctrl = (ctrl & ~MLX90632_CTRL_PWR_MASK) | MLX90632_CTRL_PWR_HALT;
		err = wr16(i2c, MLX90632_REG_CTRL, ctrl);
	}
	return err;
}
