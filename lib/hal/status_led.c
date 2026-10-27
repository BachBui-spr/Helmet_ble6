/*
 * Lop HAL - LED trang thai (xem status_led.h). Status LED patterns.
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <me54/hal/status_led.h>

#define LED_BOOT_ON_MS       100
#define LED_BOOT_GAP_MS      120
#define LED_IDLE_ON_MS       100
#define LED_IDLE_PERIOD_MS   2000
#define LED_RING_HALF_MS     250
#define LED_CALL_ON_MS       100
#define LED_CALL_PERIOD_MS   1000
#define LED_SHUTDOWN_ON_MS   1000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct k_work_delayable pattern_work;
static struct k_sem shutdown_done;
static atomic_t requested_state = ATOMIC_INIT(LED_PAT_BOOT);
static enum status_led_pattern worker_state = LED_PAT_BOOT;
static uint8_t phase;
static bool ready;

static void set_led(bool on)
{
	if (ready) {
		(void)gpio_pin_set_dt(&led, on ? 1 : 0);
	}
}

static void schedule_next(uint32_t delay_ms)
{
	k_work_reschedule(&pattern_work, K_MSEC(delay_ms));
}

static void pattern_work_handler(struct k_work *work)
{
	enum status_led_pattern state = (enum status_led_pattern)atomic_get(&requested_state);
	bool on;
	uint32_t delay_ms;

	ARG_UNUSED(work);

	if (state != worker_state) {
		worker_state = state;
		phase = 0;
	}

	switch (state) {
	case LED_PAT_BOOT:
		if (phase >= 6) {
			set_led(false);
			return;
		}
		on = (phase % 2) == 0;
		delay_ms = on ? LED_BOOT_ON_MS : LED_BOOT_GAP_MS;
		phase++;
		break;

	case LED_PAT_IDLE:
		/* One short flash every two seconds while advertising. */
		on = phase == 0;
		delay_ms = on ? LED_IDLE_ON_MS : LED_IDLE_PERIOD_MS - LED_IDLE_ON_MS;
		phase ^= 1;
		break;

	case LED_PAT_ON:
		set_led(true);
		return;

	case LED_PAT_RING:
		/* Fast 2 Hz blink for an incoming call. */
		on = phase == 0;
		delay_ms = LED_RING_HALF_MS;
		phase ^= 1;
		break;

	case LED_PAT_CALL:
		/* One short heartbeat flash per second during a call. */
		on = phase == 0;
		delay_ms = on ? LED_CALL_ON_MS : LED_CALL_PERIOD_MS - LED_CALL_ON_MS;
		phase ^= 1;
		break;

	case LED_PAT_SHUTDOWN:
		if (phase != 0) {
			set_led(false);
			k_sem_give(&shutdown_done);
			return;
		}
		on = true;
		delay_ms = LED_SHUTDOWN_ON_MS;
		phase++;
		break;

	default:
		set_led(false);
		return;
	}

	set_led(on);
	schedule_next(delay_ms);
}

int status_led_init(void)
{
	int err;

	k_work_init_delayable(&pattern_work, pattern_work_handler);
	k_sem_init(&shutdown_done, 0, 1);

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	ready = true;
	status_led_set(LED_PAT_BOOT);
	return 0;
}

void status_led_set(enum status_led_pattern state)
{
	if (!ready) {
		return;
	}

	atomic_set(&requested_state, state);
	if (state == LED_PAT_SHUTDOWN) {
		k_sem_reset(&shutdown_done);
	}
	schedule_next(0);
}

void status_led_shutdown_wait(void)
{
	if (ready) {
		if (k_sem_take(&shutdown_done, K_MSEC(LED_SHUTDOWN_ON_MS + 100)) != 0) {
			(void)k_work_cancel_delayable(&pattern_work);
			set_led(false);
		}
	}
}
