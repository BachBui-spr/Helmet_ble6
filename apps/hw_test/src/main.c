/*
 * ME54BS62 hw_test - firmware kiem tra phan cung "Helmet board" (Minew ME54BS62 / nRF54L15).
 *
 * Chuc nang:
 *   - In thong tin chip (FICR), Device ID, nguyen nhan reset.
 *   - Nhay LED Bluetooth (led0 = P2.09 -> P2.07) - cung la xung vuong de do bang scope.
 *   - Log 4 nut (Vol Up / Vol Down / Call / Power) qua input subsystem (gpio-keys).
 *   - Kiem tra cam bien MLX90632 tren I2C bit-bang.
 *   - "hw beep": phat tone ra 2 loa qua I2S -> MAX98357A.
 *   - "hw mic" : do muc tin hieu 2 micro PDM IM69D130 (L/R).
 *   - Doc nhiet do die, quang ba BLE "ME54BS62-Test".
 *   - Log + shell dong thoi tren SEGGER RTT va UART20 (P1.04 TX / P1.05 RX, 115200).
 *
 * Lenh shell: "hw info", "hw blink <ms>", "hw temp", "hw adv <on|off>",
 *             "hw mlx", "hw beep [hz] [ms]", "hw mic [ms]".
 */

#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#include <soc.h>

LOG_MODULE_REGISTER(hw_test, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS 5000
#define DEFAULT_BLINK_MS    500

/* Shell va log dung chung macro: in ra shell neu co, neu khong thi LOG_INF */
#define OUT(sh, ...)                                                                               \
	do {                                                                                       \
		if (sh) {                                                                          \
			shell_print(sh, __VA_ARGS__);                                              \
		} else {                                                                           \
			LOG_INF(__VA_ARGS__);                                                      \
		}                                                                                  \
	} while (0)

/* ---------------- LED (lay tu devicetree, bo qua neu board khong khai bao) ---------------- */

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAS_LED0 1
#else
#define HAS_LED0 0
#endif

static atomic_t blink_ms = ATOMIC_INIT(DEFAULT_BLINK_MS);
static atomic_t led_level; /* muc giu co dinh khi blink_ms = 0 ("hw led on|off") */
static atomic_t toggle_count;
static atomic_t button_count;

/* ---------------- Thong tin chip ---------------- */

static const char *reset_cause_str(uint32_t cause)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} map[] = {
		{RESET_PIN, "PIN"},           {RESET_SOFTWARE, "SOFTWARE"},
		{RESET_BROWNOUT, "BROWNOUT"}, {RESET_POR, "POWER-ON"},
		{RESET_WATCHDOG, "WATCHDOG"}, {RESET_DEBUG, "DEBUG"},
		{RESET_SECURITY, "SECURITY"}, {RESET_LOW_POWER_WAKE, "LP-WAKE"},
		{RESET_CPU_LOCKUP, "LOCKUP"}, {RESET_CLOCK, "CLOCK"},
		{RESET_HARDWARE, "HARDWARE"}, {RESET_TEMPERATURE, "TEMPERATURE"},
	};
	static char buf[96];
	size_t len = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		if ((cause & map[i].flag) && len < sizeof(buf)) {
			len += snprintk(buf + len, sizeof(buf) - len, "%s%s", len ? "|" : "",
					map[i].name);
		}
	}

	return len ? buf : "UNKNOWN";
}

static void fourcc(uint32_t v, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)((v >> (24 - 8 * i)) & 0xff);

		out[i] = isprint((unsigned char)c) ? c : '?';
	}
	out[4] = '\0';
}

static void print_chip_info(const struct shell *sh)
{
	uint8_t id[8];
	char id_str[2 * sizeof(id) + 1] = "n/a";
	ssize_t id_len = hwinfo_get_device_id(id, sizeof(id));
	char variant[5];

	if (id_len > 0) {
		bin2hex(id, id_len, id_str, sizeof(id_str));
	}
	fourcc(NRF_FICR->INFO.VARIANT, variant);

	OUT(sh, "Board     : %s", CONFIG_BOARD_TARGET);
	OUT(sh, "SoC part  : 0x%05x  variant: %s  package: 0x%x", NRF_FICR->INFO.PART, variant,
	    NRF_FICR->INFO.PACKAGE);
	OUT(sh, "Memory    : RAM %u KB, RRAM %u KB", NRF_FICR->INFO.RAM, NRF_FICR->INFO.RRAM);
	OUT(sh, "Device ID : %s", id_str);
	OUT(sh, "CPU clock : %u Hz (sys timer %u Hz)", (unsigned int)SystemCoreClock,
	    (unsigned int)sys_clock_hw_cycles_per_sec());
	OUT(sh, "Uptime    : %lld ms", k_uptime_get());
}

/* ---------------- Nhiet do die ---------------- */

static const struct device *const temp_dev = DEVICE_DT_GET(DT_NODELABEL(temp));

static int read_die_temp(struct sensor_value *val)
{
	int err;

	if (!device_is_ready(temp_dev)) {
		return -ENODEV;
	}

	err = sensor_sample_fetch(temp_dev);
	if (err) {
		return err;
	}

	return sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, val);
}

/* ---------------- Nut nhan (gpio-keys -> input subsystem, da co chong doi) ---------------- */

static const char *key_name(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_VOLUMEUP:
		return "Vol Up";
	case INPUT_KEY_VOLUMEDOWN:
		return "Vol Down";
	case INPUT_KEY_PLAYPAUSE:
		return "Call";
	case INPUT_KEY_POWER:
		return "Power";
	default:
		return "?";
	}
}

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->value) {
		atomic_inc(&button_count);
	}
	LOG_INF("Button %-8s (code %u) %s", key_name(evt->code), evt->code,
		evt->value ? "PRESSED" : "released");
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* ---------------- LED Bluetooth / xung GPIO ---------------- */

static int led_setup(void)
{
#if HAS_LED0
	int err;

	if (!gpio_is_ready_dt(&led0)) {
		LOG_ERR("led0 GPIO not ready");
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	if (err) {
		LOG_ERR("led0 configure failed (%d)", err);
		return err;
	}
	LOG_INF("led0 -> %s pin %d (toggle every %ld ms)", led0.port->name, led0.pin,
		atomic_get(&blink_ms));
#else
	LOG_WRN("No led0 alias in devicetree - GPIO toggle disabled");
#endif
	return 0;
}

/* Thread rieng xuat xung vuong tren led0: chu ky = 2 * blink_ms */
static void blink_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		long period = atomic_get(&blink_ms);

		if (period <= 0) {
#if HAS_LED0
			gpio_pin_set_dt(&led0, (int)atomic_get(&led_level));
#endif
			k_msleep(100);
			continue;
		}
#if HAS_LED0
		// gpio_pin_toggle_dt(&led0);
		gpio_pin_set_dt(&led0,0);
		atomic_inc(&toggle_count);
#endif
		k_msleep(period);
	}
}

K_THREAD_DEFINE(blink_tid, 512, blink_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);

/* ---------------- MLX90632 (I2C bit-bang) ---------------- */

#define MLX90632_ADDR        0x3a
#define MLX90632_EE_VERSION  0x240b
#define MLX90632_REG_STATUS  0x3fff

#if DT_NODE_EXISTS(DT_ALIAS(i2c_sensor))
static const struct device *const i2c_sensor = DEVICE_DT_GET(DT_ALIAS(i2c_sensor));
#else
static const struct device *const i2c_sensor;
#endif

/* MLX90632 dung dia chi thanh ghi 16-bit va du lieu 16-bit big-endian */
static int mlx_read16(uint16_t reg, uint16_t *val)
{
	uint8_t addr[2];
	uint8_t data[2];
	int err;

	sys_put_be16(reg, addr);
	err = i2c_write_read(i2c_sensor, MLX90632_ADDR, addr, sizeof(addr), data, sizeof(data));
	if (!err) {
		*val = sys_get_be16(data);
	}
	return err;
}

static int mlx_check(const struct shell *sh)
{
	uint16_t version, status;
	int err;

	if (!i2c_sensor || !device_is_ready(i2c_sensor)) {
		OUT(sh, "MLX90632: I2C bus not available");
		return -ENODEV;
	}

	err = mlx_read16(MLX90632_EE_VERSION, &version);
	if (err) {
		OUT(sh, "MLX90632 @0x%02x: NO RESPONSE (%d) - check SDA P1.13 / SCL P0.04 / pull-ups",
		    MLX90632_ADDR, err);
		return err;
	}
	(void)mlx_read16(MLX90632_REG_STATUS, &status);

	OUT(sh, "MLX90632 @0x%02x: OK  EE_VERSION=0x%04x (%s)  STATUS=0x%04x", MLX90632_ADDR,
	    version,
	    (version >> 8) == 0x01   ? "medical"
	    : (version >> 8) == 0x02 ? "consumer"
	    : (version >> 8) == 0x05 ? "extended range"
				     : "unknown type",
	    status);
	return 0;
}

/* ---------------- Loa: I2S -> 2 x MAX98357A ---------------- */

#define AUDIO_RATE       16000
#define AUDIO_BLOCK_MS   10
#define AUDIO_FRAMES     (AUDIO_RATE * AUDIO_BLOCK_MS / 1000)
#define AUDIO_BLOCK_SIZE (AUDIO_FRAMES * 2 * sizeof(int16_t)) /* stereo 16-bit */
#define AUDIO_BLOCKS     4

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(i2s20))
#define HAS_SPEAKER 1
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s20));
K_MEM_SLAB_DEFINE_STATIC(tx_slab, AUDIO_BLOCK_SIZE, AUDIO_BLOCKS, 4);
#else
#define HAS_SPEAKER 0
#endif

#if HAS_SPEAKER
static int speaker_beep(const struct shell *sh, uint32_t freq, uint32_t ms)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = AUDIO_RATE,
		.mem_slab = &tx_slab,
		.block_size = AUDIO_BLOCK_SIZE,
		.timeout = 1000,
	};
	const float step = 2.0f * 3.14159265f * (float)freq / (float)AUDIO_RATE;
	uint32_t blocks = DIV_ROUND_UP(ms, AUDIO_BLOCK_MS);
	float phase = 0.0f;
	bool started = false;
	int err;

	if (!device_is_ready(i2s_dev)) {
		OUT(sh, "I2S device not ready");
		return -ENODEV;
	}

	err = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (err) {
		OUT(sh, "i2s_configure failed (%d)", err);
		return err;
	}

	for (uint32_t b = 0; b < blocks; b++) {
		int16_t *buf;

		err = k_mem_slab_alloc(&tx_slab, (void **)&buf, K_MSEC(500));
		if (err) {
			OUT(sh, "TX slab alloc failed (%d)", err);
			break;
		}

		for (int i = 0; i < AUDIO_FRAMES; i++) {
			/* ~ -12 dBFS, cung tin hieu cho kenh L va R */
			int16_t s = (int16_t)(8000.0f * sinf(phase));

			buf[2 * i] = s;
			buf[2 * i + 1] = s;
			phase += step;
			if (phase > 2.0f * 3.14159265f) {
				phase -= 2.0f * 3.14159265f;
			}
		}

		err = i2s_write(i2s_dev, buf, AUDIO_BLOCK_SIZE);
		if (err) {
			k_mem_slab_free(&tx_slab, buf);
			OUT(sh, "i2s_write failed (%d)", err);
			break;
		}

		/* Nap san 2 block roi moi START de tranh underrun */
		if (!started && (b == 1 || b == blocks - 1)) {
			err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			if (err) {
				OUT(sh, "I2S START failed (%d)", err);
				break;
			}
			started = true;
		}
	}

	if (started) {
		i2s_trigger(i2s_dev, I2S_DIR_TX, err ? I2S_TRIGGER_DROP : I2S_TRIGGER_DRAIN);
	} else {
		i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}

	/* Cho driver phat het va tra lai cac block -> lan beep sau configure lai duoc */
	for (int i = 0; i < 100 && k_mem_slab_num_free_get(&tx_slab) < AUDIO_BLOCKS; i++) {
		k_msleep(AUDIO_BLOCK_MS);
	}

	if (!err) {
		OUT(sh, "Beep %u Hz, %u ms -> I2S BCLK P1.12 / LRCLK P1.10 / DIN P1.15", freq, ms);
	}
	return err;
}
#endif

/* ---------------- Micro: PDM -> 2 x IM69D130 ---------------- */

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(pdm20))
#define HAS_MIC 1
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(pdm20));
K_MEM_SLAB_DEFINE_STATIC(rx_slab, AUDIO_BLOCK_SIZE, 8, 4);
#else
#define HAS_MIC 0
#endif

#if HAS_MIC
static float to_dbfs(float rms)
{
	return rms > 0.5f ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
}

static int mic_measure(const struct shell *sh, uint32_t ms)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = AUDIO_RATE,
		.pcm_width = 16,
		.block_size = AUDIO_BLOCK_SIZE,
		.mem_slab = &rx_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			/* IM69D130: clock 0.4 - 3.3 MHz */
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 2,
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
					   dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT),
		},
	};
	uint32_t blocks = DIV_ROUND_UP(ms, AUDIO_BLOCK_MS);
	int64_t sum[2] = {0};
	uint64_t sum_sq[2] = {0};
	int32_t vmin[2] = {INT16_MAX, INT16_MAX};
	int32_t vmax[2] = {INT16_MIN, INT16_MIN};
	uint32_t frames = 0;
	uint32_t lr_diff = 0;
	int err;

	if (!device_is_ready(dmic_dev)) {
		OUT(sh, "PDM device not ready");
		return -ENODEV;
	}

	err = dmic_configure(dmic_dev, &cfg);
	if (err) {
		OUT(sh, "dmic_configure failed (%d)", err);
		return err;
	}
	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err) {
		OUT(sh, "DMIC START failed (%d)", err);
		return err;
	}

	/* Bo 5 block dau (~50 ms) cho bo loc PDM va micro on dinh */
	for (uint32_t b = 0; b < blocks + 5; b++) {
		void *buf;
		uint32_t size;

		err = dmic_read(dmic_dev, 0, &buf, &size, 1000);
		if (err) {
			OUT(sh, "dmic_read failed (%d) - check CLK P1.11 / DATA P1.09", err);
			break;
		}

		if (b >= 5) {
			const int16_t *s = buf;

			for (uint32_t i = 0; i + 1 < size / sizeof(int16_t); i += 2) {
				for (int ch = 0; ch < 2; ch++) {
					int32_t v = s[i + ch];

					sum[ch] += v;
					sum_sq[ch] += (int64_t)v * v;
					vmin[ch] = MIN(vmin[ch], v);
					vmax[ch] = MAX(vmax[ch], v);
				}
				lr_diff += (s[i] != s[i + 1]);
				frames++;
			}
		}
		k_mem_slab_free(&rx_slab, buf);
	}

	dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	if (err || frames == 0) {
		return err ? err : -EIO;
	}

	for (int ch = 0; ch < 2; ch++) {
		/* Tach DC (trung binh) va AC (do lech chuan) - micro that co AC, chan treo chi co DC */
		float mean = (float)sum[ch] / (float)frames;
		float var = (float)sum_sq[ch] / (float)frames - mean * mean;
		float ac_rms = var > 0.0f ? sqrtf(var) : 0.0f;

		OUT(sh, "Mic %s: DC %7.1f  AC-RMS %7.1f (%6.1f dBFS)  min %6d  max %6d  -> %s",
		    ch ? "R" : "L", (double)mean, (double)ac_rms, (double)to_dbfs(ac_rms),
		    vmin[ch], vmax[ch],
		    vmax[ch] - vmin[ch] < 4 ? "NO SIGNAL (DATA line static)" : "signal present");
	}
	/* 2 micro khac nhau tren 2 canh clock khong bao gio cho mau giong het nhau */
	OUT(sh, "L != R in %u/%u samples%s", lr_diff, frames,
	    lr_diff * 100 < frames ? "  -> L/R IDENTICAL: DATA (P1.09) not driven by 2 mics?" : "");
	OUT(sh, "(%u ms @ %u Hz. Phong yen tinh: micro that ~ -60..-75 dBFS AC; noi/vo tay -> tang)",
	    ms, AUDIO_RATE);
	return 0;
}
#endif

/* ---------------- Bluetooth LE ---------------- */

static struct k_work adv_work;
static bool adv_enabled = true;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!adv_enabled) {
		return;
	}

	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err == -EALREADY) {
		return;
	}
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}
	LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err) {
		LOG_WRN("Connection to %s failed (0x%02x %s)", addr, err, bt_hci_err_to_str(err));
		return;
	}
	LOG_INF("BLE connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BLE disconnected: %s (reason 0x%02x %s)", addr, reason,
		bt_hci_err_to_str(reason));
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		LOG_WRN("BLE security failed: level %d, err %d %s", level, err,
			bt_security_err_to_str(err));
		return;
	}
	LOG_INF("BLE security level %d (2 = encrypted, Just Works)", level);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	LOG_INF("BLE pairing complete (%s)", bonded ? "bonded, keys saved to RRAM" : "not bonded");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("BLE pairing failed: %d %s (thu xoa thiet bi tren may tinh/dien thoai roi pair lai)",
		reason, bt_security_err_to_str(reason));
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static int ble_setup(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed (err %d)", err);
		return err;
	}

	/* Nap cac bond da luu trong RRAM (phai goi sau bt_enable, truoc khi quang ba) */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}
	bt_conn_auth_info_cb_register(&auth_info_cb);

	k_work_init(&adv_work, adv_work_handler);
	k_work_submit(&adv_work);
	return 0;
}

/* ---------------- Shell ---------------- */

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_chip_info(sh);
	shell_print(sh, "Toggles   : %ld  (blink %ld ms)", atomic_get(&toggle_count),
		    atomic_get(&blink_ms));
	shell_print(sh, "Buttons   : %ld presses", atomic_get(&button_count));
	return 0;
}

static int cmd_blink(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "blink = %ld ms (0 = stop)", atomic_get(&blink_ms));
		return 0;
	}

	long ms = strtol(argv[1], NULL, 10);

	if (ms < 0 || ms > 60000) {
		shell_error(sh, "range: 0..60000 ms");
		return -EINVAL;
	}
	atomic_set(&blink_ms, ms);
	shell_print(sh, "blink -> %ld ms", ms);
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct sensor_value t;
	int err = read_die_temp(&t);

	if (err) {
		shell_error(sh, "temp read failed (%d)", err);
		return err;
	}
	shell_print(sh, "Die temp: %d.%02d C", t.val1, abs(t.val2) / 10000);
	return 0;
}

static int cmd_adv(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "adv is %s", adv_enabled ? "on" : "off");
		return 0;
	}

	if (!strcmp(argv[1], "on")) {
		adv_enabled = true;
		k_work_submit(&adv_work);
	} else if (!strcmp(argv[1], "off")) {
		adv_enabled = false;
		bt_le_adv_stop();
		LOG_INF("BLE advertising stopped");
	} else {
		shell_error(sh, "usage: hw adv <on|off>");
		return -EINVAL;
	}
	return 0;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) {
		atomic_set(&led_level, !strcmp(argv[1], "on"));
		atomic_set(&blink_ms, 0);
		shell_print(sh, "led0 (P2.07) held %s -> do bang dong ho: R16, B/C cua Q1",
			    argv[1][1] == 'n' ? "HIGH" : "LOW");
		return 0;
	}
	shell_error(sh, "usage: hw led <on|off>   (nhay lai: hw blink 500)");
	return -EINVAL;
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	if (err) {
		shell_error(sh, "unpair failed (%d)", err);
		return err;
	}
	shell_print(sh, "All bonds deleted - xoa ca thiet bi tren may tinh/dien thoai truoc khi pair lai");
	return 0;
}

static int cmd_mlx(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return mlx_check(sh);
}

static int cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
#if HAS_SPEAKER
	uint32_t freq = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000;
	uint32_t ms = argc > 2 ? strtoul(argv[2], NULL, 10) : 500;

	if (freq < 50 || freq > 7000 || ms == 0 || ms > 10000) {
		shell_error(sh, "usage: hw beep [50..7000 Hz] [1..10000 ms]");
		return -EINVAL;
	}
	return speaker_beep(sh, freq, ms);
#else
	shell_error(sh, "i2s20 not enabled in devicetree");
	return -ENOTSUP;
#endif
}

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
#if HAS_MIC
	uint32_t ms = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000;

	if (ms == 0 || ms > 10000) {
		shell_error(sh, "usage: hw mic [1..10000 ms]");
		return -EINVAL;
	}
	return mic_measure(sh, ms);
#else
	shell_error(sh, "pdm20 not enabled in devicetree");
	return -ENOTSUP;
#endif
}

SHELL_STATIC_SUBCMD_SET_CREATE(hw_cmds,
	SHELL_CMD(info, NULL, "Chip/board info", cmd_info),
	SHELL_CMD_ARG(blink, NULL, "Set led0 toggle period: hw blink <ms> (0 = stop)", cmd_blink,
		      1, 1),
	SHELL_CMD(temp, NULL, "Read die temperature", cmd_temp),
	SHELL_CMD_ARG(adv, NULL, "BLE advertising: hw adv <on|off>", cmd_adv, 1, 1),
	SHELL_CMD_ARG(led, NULL, "Hold led0 level: hw led <on|off>", cmd_led, 2, 0),
	SHELL_CMD(unpair, NULL, "Delete all BLE bonds", cmd_unpair),
	SHELL_CMD(mlx, NULL, "Probe MLX90632 IR temperature sensor (I2C 0x3A)", cmd_mlx),
	SHELL_CMD_ARG(beep, NULL, "Play sine tone on speakers: hw beep [hz] [ms]", cmd_beep, 1, 2),
	SHELL_CMD_ARG(mic, NULL, "Measure microphone level L/R: hw mic [ms]", cmd_mic, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(hw, &hw_cmds, "Helmet board hardware test commands", NULL);

/* ---------------- main ---------------- */

int main(void)
{
	uint32_t cause = 0;

	LOG_INF("=== ME54BS62 hw_test (nRF54L15) ===");
	print_chip_info(NULL);

	if (hwinfo_get_reset_cause(&cause) == 0) {
		LOG_INF("Reset cause: %s (0x%08x)", reset_cause_str(cause), cause);
		hwinfo_clear_reset_cause();
	}

	led_setup();
	mlx_check(NULL);
	ble_setup();

	while (1) {
		struct sensor_value t;

		k_msleep(HEARTBEAT_PERIOD_MS);

		if (read_die_temp(&t) == 0) {
			LOG_INF("heartbeat: up %lld s, toggles %ld, buttons %ld, die %d.%02d C",
				k_uptime_get() / 1000, atomic_get(&toggle_count),
				atomic_get(&button_count), t.val1, abs(t.val2) / 10000);
		} else {
			LOG_INF("heartbeat: up %lld s, toggles %ld, buttons %ld",
				k_uptime_get() / 1000, atomic_get(&toggle_count),
				atomic_get(&button_count));
		}
	}

	return 0;
}
