/*
 * Lop CORE - cong thuc MLX90632 (ham thuan). I/O I2C o lib/hal/mlx90632.c.
 *
 * Cong thuc: mlx90632_calc_temp_ambient / mlx90632_calc_temp_object cua Melexis, giu ca phep
 * ep kieu int32_t cua tham so tien xu ly de ket qua khop bo kiem thu cua thu vien goc.
 */

#include <math.h>

#include <me54/core/mlx90632_calc.h>

/* ---------------- Tinh toan ---------------- */

double mlx90632_preprocess_ambient(int16_t ambient_new, int16_t ambient_old, int16_t Gb)
{
	const double kGb = (double)Gb / 1024.0;
	const double VR_Ta = ambient_old + kGb * (ambient_new / 12.0);

	return ((ambient_new / 12.0) / VR_Ta) * 524288.0;
}

double mlx90632_preprocess_object(int16_t object_new, int16_t object_old, int16_t ambient_new,
				  int16_t ambient_old, int16_t Ka)
{
	const double kKa = (double)Ka / 1024.0;
	const double VR_IR = ambient_old + kKa * (ambient_new / 12.0);

	return ((((object_new + object_old) / 2) / 12.0) / VR_IR) * 524288.0;
}

double mlx90632_calc_ambient(const struct mlx90632_cal *c, const struct mlx90632_raw *r)
{
	const double AMB = mlx90632_preprocess_ambient(r->ambient_new, r->ambient_old, c->Gb);
	const double Asub = (double)c->P_T / 17592186044416.0; /* 2^44 */
	const double Bsub = AMB - ((double)c->P_R / 256.0);
	const double Ablock = Asub * (Bsub * Bsub);
	const double Bblock = (Bsub / (double)c->P_G) * 1048576.0; /* 2^20 */
	const double Cblock = (double)c->P_O / 256.0;

	return Bblock + Ablock + Cblock;
}

static double object_iteration(double prev, int32_t object, double TAdut,
			       const struct mlx90632_cal *c, double emissivity)
{
	const double Ha_c = c->Ha / 16384.0;
	const double Hb_c = c->Hb / 1024.0;
	const double calcedGa = ((double)c->Ga * (prev - 25)) / 68719476736.0;  /* 2^36 */
	const double calcedGb = ((double)c->Fb * (TAdut - 25)) / 68719476736.0; /* 2^36 */
	/* = Fa/2^46 * Ha/2^14 * (1 + Ga' + Gb') */
	const double alpha = ((double)c->Fa * Ha_c * (1.0 + calcedGa + calcedGb)) /
			     70368744177664.0; /* 2^46 */
	const double calcedFa = object / (emissivity * alpha);
	const double t = TAdut + 273.15;
	const double first_sqrt = sqrt(calcedFa + t * t * t * t);

	return sqrt(first_sqrt) - 273.15 - Hb_c;
}

double mlx90632_calc_object(const struct mlx90632_cal *c, const struct mlx90632_raw *r,
			    double emissivity)
{
	/* Thu vien goc truyen gia tri tien xu ly qua tham so int32_t (cat phan le) */
	const int32_t object = (int32_t)mlx90632_preprocess_object(
		r->object_new, r->object_old, r->ambient_new, r->ambient_old, c->Ka);
	const int32_t ambient =
		(int32_t)mlx90632_preprocess_ambient(r->ambient_new, r->ambient_old, c->Gb);
	const double kEa = (double)c->Ea / 65536.0;
	const double kEb = (double)c->Eb / 256.0;
	const double TAdut = ((double)ambient - kEb) / kEa + 25;
	double temp = 25.0;

	if (emissivity <= 0.0 || emissivity > 1.0) {
		emissivity = 1.0;
	}
	for (int i = 0; i < 5; i++) {
		temp = object_iteration(temp, object, TAdut, c, emissivity);
	}
	return temp;
}
