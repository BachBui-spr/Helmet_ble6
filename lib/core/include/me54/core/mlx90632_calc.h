/*
 * Lop CORE - MLX90632: cong thuc chinh thuc cua Melexis (mlx90632-library, Apache-2.0), ham thuan
 * -> bien dich va kiem thu duoc tren PC (17 vector TestDSP cua Melexis).
 */

#ifndef ME54_CORE_MLX90632_CALC_H_
#define ME54_CORE_MLX90632_CALC_H_

#include <stdint.h>

struct mlx90632_cal {
	int32_t P_R;
	int32_t P_G;
	int32_t P_T;
	int32_t P_O;
	int32_t Ea;
	int32_t Eb;
	int32_t Fa;
	int32_t Fb;
	int32_t Ga;
	int16_t Gb;
	int16_t Ka;
	int16_t Ha;
	int16_t Hb;
};

struct mlx90632_raw {
	int16_t ambient_new;
	int16_t ambient_old;
	int16_t object_new;
	int16_t object_old;
	uint8_t cycle_pos;
};

/* ---- Tinh toan (ham thuan) ---- */
double mlx90632_preprocess_ambient(int16_t ambient_new, int16_t ambient_old, int16_t Gb);
double mlx90632_preprocess_object(int16_t object_new, int16_t object_old, int16_t ambient_new,
				  int16_t ambient_old, int16_t Ka);
double mlx90632_calc_ambient(const struct mlx90632_cal *c, const struct mlx90632_raw *r);
double mlx90632_calc_object(const struct mlx90632_cal *c, const struct mlx90632_raw *r,
			    double emissivity);

#endif /* ME54_CORE_MLX90632_CALC_H_ */
