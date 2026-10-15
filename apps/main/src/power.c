/*
 * Helmet - nguon: bat/tat, tu tat, quang ba cham, do VDD.
 *
 * Phan cung: LDO AP2112K co EN noi thang VIN -> mach luon co 3.3 V khi co pin. "Tat may" la
 * System OFF cua nRF54L15 (sys_poweroff), thuc day bang muc "nhan" cua nut Power P0.03 ->
 * chip reset va chay lai tu dau main().
 *
 * - power_boot_check(): thuc tu System OFF -> nut phai con giu du 2 s (tinh tu luc reset),
 *   nha som thi tat lai ngay, khong am bao (chong bat nham trong tui).
 * - power_off(): quy trinh 13 buoc (docs/HELMET_THIET_KE_CHUC_NANG_MOI.md muc 6.4).
 * - Hen gio: 30 s quang ba nhanh roi chuyen 150 ms; tu tat sau N phut khong ket noi.
 * - VDD: LDO sut ap khi pin yeu -> VDD tut theo pin. Do moi 60 s bang SAADC (kenh VDD noi).
 *   Canh bao khi < 3.20 V. Tu tat khi < 3.00 V chi bat sau khi da do VDD that tren mach
 *   (LOWBATT_POWER_OFF), tranh tat nham neu kenh do chua dung.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#include "helmet.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define POWER_ON_HOLD_MS  2000
#define ADV_FAST_S        30
#define RELEASE_WAIT_MS   10000
#define PROMPT_WAIT_MS    800
#define VDD_PERIOD_S      60
#define VDD_FIRST_S       20
#define VDD_WARN_MV       3200
#define VDD_OFF_MV        3000
#define VDD_WARN_EVERY    5 /* lan do (~5 phut) giua hai lan canh bao */
#define LOWBATT_POWER_OFF 0
#define LOG_FLUSH_WAIT_US 100000 /* cho J-Link doc het RTT truoc khi tat (RAM se mat) */

static const struct gpio_dt_spec power_btn = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_power), gpios);
static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart20));
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

static uint32_t boot_cause;
static bool woke_by_button;

static void idle_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	app_post(EVT_IDLE_TIMEOUT, 0, 0, 0);
}

static void adv_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	app_post(EVT_ADV_SLOWDOWN, 0, 0, 0);
}

static K_TIMER_DEFINE(idle_timer, idle_expiry, NULL);
static K_TIMER_DEFINE(adv_timer, adv_expiry, NULL);

/* ---------------- Tat that su ---------------- */

static FUNC_NORETURN void enter_system_off(bool wait_release)
{
	if (wait_release) {
		/* Chan thuc theo MUC "nhan": con dang giu thi chip thuc ngay lap tuc */
		for (int i = 0; i < RELEASE_WAIT_MS / 20 && gpio_pin_get_dt(&power_btn) > 0; i++) {
			k_msleep(20);
		}
	}

	/* Log dang o che do ghi tre (thread log thuc ~1 s/lan): day het tin nhan dang cho ra
	 * RTT/UART ngay, chuyen sang ghi tuc thoi, roi cho J-Link kip doc bo dem RTT.
	 * System OFF lam mat RAM -> neu khong, cac dong log cuoi cua qua trinh tat se mat.
	 */
	LOG_PANIC();
	k_busy_wait(LOG_FLUSH_WAIT_US);

	(void)gpio_pin_interrupt_configure_dt(&power_btn, GPIO_INT_LEVEL_ACTIVE);
	if (device_is_ready(uart_dev)) {
		(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
	}
	(void)hwinfo_clear_reset_cause();
	sys_poweroff();
}

/* ---------------- Khoi dong ---------------- */

bool power_boot_check(void)
{
	(void)hwinfo_get_reset_cause(&boot_cause);

	if (boot_cause & RESET_LOW_POWER_WAKE) {
		/* Thuc tu System OFF do nut Power: phai giu du 2 s */
		while (k_uptime_get() < POWER_ON_HOLD_MS) {
			if (gpio_pin_get_dt(&power_btn) <= 0) {
				enter_system_off(false);
			}
			k_msleep(20);
		}
		woke_by_button = true;
	}

	(void)hwinfo_clear_reset_cause();
	LOG_INF("Boot: reset cause 0x%08x%s", boot_cause,
		woke_by_button ? " (Power button held 2 s -> ON)" : "");

	return woke_by_button || gpio_pin_get_dt(&power_btn) > 0;
}

/* ---------------- Quy trinh tat ---------------- */

static const char *reason_str(enum power_off_reason why)
{
	switch (why) {
	case OFF_BUTTON:
		return "button";
	case OFF_IDLE:
		return "idle timeout";
	case OFF_LOW_BATTERY:
		return "low battery";
	case OFF_SHELL:
		return "shell";
	default:
		return "?";
	}
}

FUNC_NORETURN void power_off(enum power_off_reason why)
{
	const enum helmet_call call = call_ctrl_state();

	LOG_INF("Power off (%s)", reason_str(why));
	k_timer_stop(&idle_timer);
	k_timer_stop(&adv_timer);

	/* 2. Ket thuc / tu choi cuoc goi */
	if (call == CALL_INCOMING) {
		(void)call_ctrl_reject();
		k_msleep(300);
	} else if (call != CALL_NONE) {
		(void)call_ctrl_hangup();
		k_msleep(300);
	}

	/* 3. Am bao tat may */
	audio_out_prompt(PROMPT_POWER_OFF);
	for (int i = 0; i < PROMPT_WAIT_MS / 20 && audio_out_prompt_busy(); i++) {
		k_msleep(20);
	}
	k_msleep(100); /* phan cuoi am bao con trong hang doi I2S */

	/* 4-5. Ngat ket noi (ly do 0x15: tat nguon), dung quang ba */
	app_bt_shutdown();

	/* 6-7. Dung am thanh, micro, cam bien */
	voice_stop();
	audio_out_shutdown();
	thermo_sleep();

	/* 8. Luu cai dat */
	helmet_cfg_save();

	LOG_INF("System OFF - hold Power 2 s to turn on");

	/* 9-13 (enter_system_off day het log truoc khi tat) */
	enter_system_off(why == OFF_BUTTON);
}

/* ---------------- Hen gio ---------------- */

void power_state_changed(enum helmet_state s)
{
	if (s == HS_IDLE) {
		k_timer_start(&adv_timer, K_SECONDS(ADV_FAST_S), K_NO_WAIT);
		if (helmet_cfg.autooff_min > 0) {
			k_timer_start(&idle_timer, K_MINUTES(helmet_cfg.autooff_min), K_NO_WAIT);
		} else {
			k_timer_stop(&idle_timer);
		}
	} else {
		k_timer_stop(&adv_timer);
		k_timer_stop(&idle_timer);
	}
}

void power_activity(void)
{
	if (app_state() == HS_IDLE && helmet_cfg.autooff_min > 0) {
		k_timer_start(&idle_timer, K_MINUTES(helmet_cfg.autooff_min), K_NO_WAIT);
	}
}

void power_autooff_changed(void)
{
	if (app_state() == HS_IDLE) {
		power_state_changed(HS_IDLE);
	}
}

/* ---------------- Do VDD ---------------- */

static struct adc_channel_cfg vdd_ch = {
	.gain = ADC_GAIN_1_4,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = 0,
	.input_positive = NRF_SAADC_VDD,
};
static bool vdd_ok;
static int vdd_last_mv = -1;
static uint8_t low_count;
static uint8_t off_count;
static uint8_t warn_skip;

int power_vdd_mv(void)
{
	int16_t sample = 0;
	int32_t mv;
	struct adc_sequence seq = {
		.channels = BIT(0),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12,
	};

	if (!vdd_ok || adc_read(adc_dev, &seq) != 0) {
		return -EIO;
	}
	mv = sample;
	if (adc_raw_to_millivolts(adc_ref_internal(adc_dev), vdd_ch.gain, seq.resolution, &mv)) {
		return -EIO;
	}
	vdd_last_mv = (int)mv;
	return (int)mv;
}

static void vdd_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(vdd_work, vdd_work_fn);

static void vdd_work_fn(struct k_work *work)
{
	const int mv = power_vdd_mv();

	ARG_UNUSED(work);

	if (mv >= 1500 && mv <= 3800) {
		low_count = (mv < VDD_WARN_MV) ? low_count + 1 : 0;
		off_count = (mv < VDD_OFF_MV) ? off_count + 1 : 0;

		if (low_count >= 3) {
			if (warn_skip == 0) {
				LOG_WRN("Battery low: VDD %d mV", mv);
				app_post(EVT_LOW_BATTERY, 1, 0, (uint32_t)mv);
				warn_skip = VDD_WARN_EVERY;
			} else {
				warn_skip--;
			}
		}
		if (LOWBATT_POWER_OFF && off_count >= 3) {
			app_post(EVT_LOW_BATTERY, 2, 0, (uint32_t)mv);
		}
	}

	k_work_reschedule(&vdd_work, K_SECONDS(VDD_PERIOD_S));
}

static int power_init(void)
{
	int err;

	if (!device_is_ready(adc_dev)) {
		return 0;
	}
	err = adc_channel_setup(adc_dev, &vdd_ch);
	if (err) {
		LOG_WRN("VDD channel setup failed (%d) - battery warning disabled", err);
		return 0;
	}
	vdd_ok = true;
	k_work_reschedule(&vdd_work, K_SECONDS(VDD_FIRST_S));
	return 0;
}

SYS_INIT(power_init, APPLICATION, 90);

void power_print_status(const struct shell *sh)
{
	const uint32_t idle_left = k_timer_remaining_get(&idle_timer);

	shell_print(sh, "Power     : state %s, boot cause 0x%08x%s", app_state_str(app_state()),
		    boot_cause, woke_by_button ? " (Power button)" : "");
	shell_print(sh, "Auto off  : %u min%s, %u s left", helmet_cfg.autooff_min,
		    helmet_cfg.autooff_min ? "" : " (disabled)", idle_left / 1000U);
	if (vdd_ok) {
		shell_print(sh, "VDD       : %d mV (warn < %d, low count %u)", vdd_last_mv,
			    VDD_WARN_MV, low_count);
	} else {
		shell_print(sh, "VDD       : not available");
	}
}
