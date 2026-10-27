/*
 * Lop HAL - nguon cua board (xem power_hw.h).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#if defined(CONFIG_ADC)
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#endif

#include <me54/hal/power_hw.h>

#define POWER_ON_HOLD_MS  2000
#define RELEASE_WAIT_MS   10000
#define LOG_FLUSH_WAIT_US 100000 /* cho J-Link doc het RTT truoc khi tat (RAM se mat) */

static const struct gpio_dt_spec power_btn = GPIO_DT_SPEC_GET(DT_NODELABEL(btn_power), gpios);
#if defined(CONFIG_PM_DEVICE)
static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart20));
#endif

FUNC_NORETURN void power_hw_system_off(bool wait_release)
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
#if defined(CONFIG_PM_DEVICE)
	if (device_is_ready(uart_dev)) {
		(void)pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
	(void)hwinfo_clear_reset_cause();
	sys_poweroff();
}

bool power_hw_boot_check(uint32_t *cause, bool *woke_by_button)
{
	*cause = 0;
	*woke_by_button = false;
	(void)hwinfo_get_reset_cause(cause);

	if (*cause & RESET_LOW_POWER_WAKE) {
		/* Thuc tu System OFF do nut Power: phai giu du 2 s */
		while (k_uptime_get() < POWER_ON_HOLD_MS) {
			if (gpio_pin_get_dt(&power_btn) <= 0) {
				power_hw_system_off(false); /* nha som: bat nham, tat lai */
			}
			k_msleep(20);
		}
		*woke_by_button = true;
	}

	(void)hwinfo_clear_reset_cause();
	return *woke_by_button || gpio_pin_get_dt(&power_btn) > 0;
}

#if defined(CONFIG_ADC)
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
static struct adc_channel_cfg vdd_ch = {
	.gain = ADC_GAIN_1_4,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = 0,
	.input_positive = NRF_SAADC_VDD,
};
static bool vdd_ok;

int power_hw_vdd_init(void)
{
	int err;

	if (!device_is_ready(adc_dev)) {
		return -ENODEV;
	}
	err = adc_channel_setup(adc_dev, &vdd_ch);
	vdd_ok = (err == 0);
	return err;
}

int power_hw_vdd_mv(void)
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
	return (int)mv;
}
#else
int power_hw_vdd_init(void)
{
	return -ENODEV;
}

int power_hw_vdd_mv(void)
{
	return -EIO;
}
#endif /* CONFIG_ADC */
