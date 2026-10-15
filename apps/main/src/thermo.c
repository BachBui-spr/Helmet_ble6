/*
 * Helmet - nhiet do hong ngoai MLX90632.
 *
 * - Thread rieng uu tien THAP (preempt 10): I2C bit-bang la vong cho ban cua CPU (~4 ms moi
 *   lan doc RAM); chay trong sysworkq (coop, khong bi chiem quyen) se chan audio thread.
 * - Moi 2 s: do 1 luot (sleeping step, ~1 s) -> kiem tra hop le -> trung vi 5 mau
 *   -> trung binh truot -> so nguong co tre (3 lan lien tiep, tre 0.5 do).
 * - Health Thermometer Service (0x1809): Temperature Measurement (0x2A1C, indicate) moi 10 s
 *   va ngay khi co canh bao; Temperature Type (0x2A1D) = 0x02 "Body (general)".
 * - Khong co cam bien: thu lai moi 30 s, he thong van chay binh thuong.
 * Nhiet do trao doi giua cac module bang milli-do C (so nguyen).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "helmet.h"
#include "mlx90632.h"

LOG_MODULE_REGISTER(thermo, LOG_LEVEL_INF);

#define T_MEAS_MS      2000
#define HTS_PERIOD_MS  10000
#define PROBE_RETRY_MS 30000
#define MEDIAN_N       5
#define ALARM_COUNT    3
#define ALARM_HYST_MC  500

#define THERMO_STACK 2560
#define THERMO_PRIO  K_PRIO_PREEMPT(10)

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_ALIAS(i2c_sensor));

static K_SEM_DEFINE(start_sem, 0, 1);
static atomic_t sleep_req;

static struct k_spinlock lock;
static struct helmet_temp cur;
static int64_t cur_time;
static struct mlx90632_cal cal;
static struct mlx90632_raw last_raw;
static int32_t last_obj_raw_mc;
static int32_t last_amb_raw_mc;
static uint16_t version;
static bool present;
static uint32_t dataset_ms = 1000;
static uint32_t meas_count;
static uint32_t err_count;

static int32_t hist_obj[MEDIAN_N];
static int32_t hist_amb[MEDIAN_N];
static uint8_t hist_n;
static uint8_t hist_i;
static int32_t ema_obj;
static int32_t ema_amb;
static uint8_t over_cnt;
static uint8_t under_cnt;
static bool alarm_on;

/* ---------------- Health Thermometer Service ---------------- */

static bool hts_enabled;
static atomic_t hts_busy;
static struct bt_gatt_indicate_params ind_params;
static uint8_t htm[5];
static const uint8_t temp_type = 0x02; /* Body (general) */

static void htm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	hts_enabled = (value == BT_GATT_CCC_INDICATE);
	LOG_INF("HTS indications %s", hts_enabled ? "enabled" : "disabled");
}

static ssize_t read_temp_type(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &temp_type, sizeof(temp_type));
}

/* Bat ma hoa giong cac dich vu LE Audio: may chua ghep doi bi Helmet ngat (main.c) */
BT_GATT_SERVICE_DEFINE(hts_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HTS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HTS_MEASUREMENT, BT_GATT_CHRC_INDICATE, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(htm_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CHARACTERISTIC(BT_UUID_HTS_TEMP_TYP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
			       read_temp_type, NULL, NULL),
);

static void indicate_done(struct bt_gatt_indicate_params *params)
{
	ARG_UNUSED(params);
	atomic_clear(&hts_busy);
}

/* IEEE-11073 FLOAT 32 bit: dinh tri 24 bit co dau + so mu co so 10 (8 bit) */
static void hts_indicate(int32_t temp_mc)
{
	const int32_t mantissa = temp_mc / 10; /* 0.01 do C */

	if (!hts_enabled || atomic_get(&hts_busy)) {
		return;
	}

	htm[0] = 0x00; /* do C, khong co timestamp, khong co loai (da co dac tinh 0x2A1D) */
	sys_put_le24((uint32_t)mantissa & 0xFFFFFF, &htm[1]);
	htm[4] = (uint8_t)-2;

	ind_params.attr = &hts_svc.attrs[2];
	ind_params.func = NULL;
	ind_params.destroy = indicate_done;
	ind_params.data = htm;
	ind_params.len = sizeof(htm);

	atomic_set(&hts_busy, 1);
	if (bt_gatt_indicate(NULL, &ind_params) != 0) {
		atomic_clear(&hts_busy);
	}
}

/* ---------------- Loc ---------------- */

static int32_t median(const int32_t *v, uint8_t n)
{
	int32_t s[MEDIAN_N];

	memcpy(s, v, n * sizeof(int32_t));
	for (uint8_t i = 1; i < n; i++) {
		for (uint8_t j = i; j > 0 && s[j - 1] > s[j]; j--) {
			const int32_t t = s[j];

			s[j] = s[j - 1];
			s[j - 1] = t;
		}
	}
	return s[n / 2];
}

static void filter_and_alarm(int32_t obj_mc, int32_t amb_mc)
{
	k_spinlock_key_t key;
	int32_t obj_med;
	int32_t amb_med;
	const int32_t thr = helmet_cfg.temp_alarm_mc;

	hist_obj[hist_i] = obj_mc;
	hist_amb[hist_i] = amb_mc;
	hist_i = (hist_i + 1) % MEDIAN_N;
	if (hist_n < MEDIAN_N) {
		hist_n++;
	}
	obj_med = median(hist_obj, hist_n);
	amb_med = median(hist_amb, hist_n);

	if (hist_n == 1) {
		ema_obj = obj_med;
		ema_amb = amb_med;
	} else {
		/* alpha = 0.3 */
		ema_obj += (obj_med - ema_obj) * 3 / 10;
		ema_amb += (amb_med - ema_amb) * 3 / 10;
	}

	key = k_spin_lock(&lock);
	cur.object_mc = ema_obj;
	cur.ambient_mc = ema_amb;
	cur.valid = true;
	cur_time = k_uptime_get();
	k_spin_unlock(&lock, key);

	if (!alarm_on) {
		over_cnt = (ema_obj >= thr) ? over_cnt + 1 : 0;
		if (over_cnt >= ALARM_COUNT) {
			alarm_on = true;
			under_cnt = 0;
			LOG_WRN("Temperature alarm ON: %d.%03d C", ema_obj / 1000,
				(ema_obj < 0 ? -ema_obj : ema_obj) % 1000);
			app_post(EVT_TEMP_ALARM, 1, 0, (uint32_t)ema_obj);
			hts_indicate(ema_obj);
		}
	} else {
		under_cnt = (ema_obj < thr - ALARM_HYST_MC) ? under_cnt + 1 : 0;
		if (under_cnt >= ALARM_COUNT) {
			alarm_on = false;
			over_cnt = 0;
			LOG_INF("Temperature alarm OFF");
			app_post(EVT_TEMP_ALARM, 0, 0, (uint32_t)ema_obj);
		}
	}
	cur.alarm = alarm_on;
}

/* ---------------- Thread ---------------- */

static bool sensor_setup(void)
{
	int err;

	if (!device_is_ready(i2c_dev)) {
		return false;
	}
	if (mlx90632_probe(i2c_dev, &version) != 0) {
		return false;
	}
	if ((version & 0x00FF) != MLX90632_DSPV5) {
		LOG_WRN("MLX90632 EE_VERSION 0x%04x: not DSPv5, results may be wrong", version);
	}
	err = mlx90632_read_cal(i2c_dev, &cal);
	if (err == 0) {
		err = mlx90632_config(i2c_dev, &dataset_ms);
	}
	if (err) {
		LOG_ERR("MLX90632 setup failed (%d)", err);
		return false;
	}
	LOG_INF("MLX90632 found: EE_VERSION 0x%04x, measurement %u ms", version, dataset_ms);
	return true;
}

static void thermo_thread(void *p1, void *p2, void *p3)
{
	int64_t last_hts = 0;
	bool warned = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&start_sem, K_FOREVER);

	while (!atomic_get(&sleep_req)) {
		struct mlx90632_raw raw;
		double emis;
		double ta;
		double to;
		int err;

		if (!present) {
			present = sensor_setup();
			if (!present) {
				if (!warned) {
					LOG_INF("MLX90632 not present - retry every %u s",
						PROBE_RETRY_MS / 1000U);
					warned = true;
				}
				k_msleep(PROBE_RETRY_MS);
				continue;
			}
		}

		err = mlx90632_measure(i2c_dev, dataset_ms, &raw);
		if (err) {
			err_count++;
			LOG_WRN("MLX90632 measure failed (%d)", err);
			if (err_count % 5 == 0) {
				present = false; /* doc lai hieu chuan / cau hinh */
			}
			k_msleep(T_MEAS_MS);
			continue;
		}

		emis = helmet_cfg.temp_emis_permille / 1000.0;
		ta = mlx90632_calc_ambient(&cal, &raw);
		to = mlx90632_calc_object(&cal, &raw, emis);
		last_raw = raw;
		last_amb_raw_mc = (int32_t)(ta * 1000.0);
		last_obj_raw_mc = (int32_t)(to * 1000.0);
		meas_count++;

		if (ta < -20.0 || ta > 85.0 || to < -20.0 || to > 100.0 || to != to) {
			err_count++;
		} else {
			filter_and_alarm(last_obj_raw_mc, last_amb_raw_mc);
		}

		if (k_uptime_get() - last_hts >= HTS_PERIOD_MS && cur.valid) {
			hts_indicate(cur.object_mc);
			last_hts = k_uptime_get();
		}

		k_msleep(T_MEAS_MS > dataset_ms ? T_MEAS_MS - dataset_ms : 100);
	}

	if (present) {
		(void)mlx90632_halt(i2c_dev);
	}
	LOG_INF("Thermo stopped");
}

K_THREAD_DEFINE(thermo_tid, THERMO_STACK, thermo_thread, NULL, NULL, NULL, THERMO_PRIO,
		K_FP_REGS, 0);

/* ---------------- API ---------------- */

int thermo_init(void)
{
	k_sem_give(&start_sem);
	return 0;
}

void thermo_get(struct helmet_temp *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = cur;
	out->age_ms = cur.valid ? (uint32_t)(k_uptime_get() - cur_time) : 0;
	k_spin_unlock(&lock, key);
}

void thermo_sleep(void)
{
	atomic_set(&sleep_req, 1);
	if (present) {
		(void)mlx90632_halt(i2c_dev);
	}
}

static void print_mc(const struct shell *sh, const char *name, int32_t mc)
{
	shell_print(sh, "%s%d.%02d C", name, mc / 1000, (mc < 0 ? -mc : mc) % 1000 / 10);
}

int thermo_shell(const struct shell *sh, size_t argc, char **argv)
{
	struct helmet_temp t;

	if (argc >= 2 && strcmp(argv[1], "raw") == 0) {
		shell_print(sh, "Last raw: ambient new %d old %d, object new %d old %d, cycle %u",
			    last_raw.ambient_new, last_raw.ambient_old, last_raw.object_new,
			    last_raw.object_old, last_raw.cycle_pos);
		print_mc(sh, "Unfiltered object : ", last_obj_raw_mc);
		print_mc(sh, "Unfiltered ambient: ", last_amb_raw_mc);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "cal") == 0) {
		shell_print(sh, "EE_VERSION 0x%04x, dataset %u ms", version, dataset_ms);
		shell_print(sh, "P_R %d P_G %d P_T %d P_O %d", cal.P_R, cal.P_G, cal.P_T, cal.P_O);
		shell_print(sh, "Ea %d Eb %d Fa %d Fb %d Ga %d", cal.Ea, cal.Eb, cal.Fa, cal.Fb,
			    cal.Ga);
		shell_print(sh, "Gb %d Ka %d Ha %d Hb %d", cal.Gb, cal.Ka, cal.Ha, cal.Hb);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "emis") == 0) {
		const long v = strtol(argv[2], NULL, 10);

		if (v < 100 || v > 1000) {
			shell_error(sh, "usage: helmet temp emis <100..1000> (per mille, 980 = 0.98)");
			return -EINVAL;
		}
		helmet_cfg.temp_emis_permille = (uint16_t)v;
		helmet_cfg_changed();
		shell_print(sh, "Emissivity %ld/1000", v);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "alarm") == 0) {
		const long v = strtol(argv[2], NULL, 10);

		if (v < 20 || v > 60) {
			shell_error(sh, "usage: helmet temp alarm <20..60> (degrees C)");
			return -EINVAL;
		}
		helmet_cfg.temp_alarm_mc = (int32_t)v * 1000;
		helmet_cfg_changed();
		shell_print(sh, "Alarm threshold %ld C", v);
		return 0;
	}

	thermo_get(&t);
	if (!present) {
		shell_print(sh, "MLX90632  : not present (I2C SDA P1.13 / SCL P0.04, addr 0x%02x)",
			    MLX90632_ADDR);
		return 0;
	}
	shell_print(sh, "MLX90632  : EE_VERSION 0x%04x, %u measurements, %u errors", version,
		    meas_count, err_count);
	if (t.valid) {
		print_mc(sh, "Object    : ", t.object_mc);
		print_mc(sh, "Ambient   : ", t.ambient_mc);
		shell_print(sh, "Age       : %u ms, alarm %s (threshold %d C), emissivity %u/1000",
			    t.age_ms, t.alarm ? "ON" : "off", helmet_cfg.temp_alarm_mc / 1000,
			    helmet_cfg.temp_emis_permille);
	}
	shell_print(sh, "HTS       : indications %s", hts_enabled ? "on" : "off");
	return 0;
}
