/*
 * Lop HAL - I2S20 (xem i2s_out.h).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>

#include <nrfx_i2s.h>

#include <me54/hal/i2s_out.h>

K_MEM_SLAB_DEFINE_STATIC(i2s_slab, I2S_OUT_BLOCK_BYTES, I2S_OUT_BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s20));
static bool configured;

int i2s_out_init(void)
{
	return device_is_ready(i2s_dev) ? 0 : -ENODEV;
}

int i2s_out_configure(uint32_t rate, uint16_t block_samples, int32_t timeout_ms)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = rate,
		.mem_slab = &i2s_slab,
		.block_size = block_samples * 2 * sizeof(int16_t),
		.timeout = timeout_ms,
	};
	const int err = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);

	configured = (err == 0);
	return err;
}

bool i2s_out_configured(void)
{
	return configured;
}

uint8_t i2s_out_blocks_in_use(void)
{
	return I2S_OUT_BLOCK_COUNT - k_mem_slab_num_free_get(&i2s_slab);
}

void i2s_out_wait_released(uint32_t tries, uint32_t step_ms)
{
	for (uint32_t i = 0; i < tries && i2s_out_blocks_in_use() > 0; i++) {
		k_msleep(step_ms);
	}
}

void i2s_out_halt(void)
{
	if (!configured) {
		return;
	}
	if (i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP) != 0) {
		/* Trang thai ERROR (underrun) can PREPARE de ve READY */
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	}
	i2s_out_wait_released(50, 2);
}

int i2s_out_start(void)
{
	return i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
}

int i2s_out_drain(void)
{
	return i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
}

int i2s_out_drop(void)
{
	return i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
}

int i2s_out_alloc(void **blk, k_timeout_t timeout)
{
	return k_mem_slab_alloc(&i2s_slab, blk, timeout);
}

void i2s_out_free(void *blk)
{
	k_mem_slab_free(&i2s_slab, blk);
}

int i2s_out_write(void *blk, size_t size)
{
	return i2s_write(i2s_dev, blk, size);
}

static uint32_t ratio_value(nrf_i2s_ratio_t r)
{
	switch (r) {
	case NRF_I2S_RATIO_32X:
		return 32;
	case NRF_I2S_RATIO_48X:
		return 48;
	case NRF_I2S_RATIO_64X:
		return 64;
	case NRF_I2S_RATIO_96X:
		return 96;
	case NRF_I2S_RATIO_128X:
		return 128;
	case NRF_I2S_RATIO_192X:
		return 192;
	case NRF_I2S_RATIO_256X:
		return 256;
	case NRF_I2S_RATIO_384X:
		return 384;
	case NRF_I2S_RATIO_512X:
		return 512;
	default:
		return 0;
	}
}

static uint64_t gcd64(uint64_t a, uint64_t b)
{
	while (b != 0) {
		uint64_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

int i2s_out_actual_rate(uint32_t rate, struct i2s_out_rate *out)
{
	const nrfx_i2s_clk_params_t p = {
		.base_clock_freq = 32 * 1000 * 1000UL,
		.transfer_rate = rate,
		.swidth = NRF_I2S_SWIDTH_16BIT,
		.allow_bypass = IS_ENABLED(CONFIG_I2S_NRFX_ALLOW_MCK_BYPASS),
	};
	nrfx_i2s_prescalers_t pre = {0};
	uint32_t ratio;
	uint32_t div = 0;
	uint64_t num;
	uint64_t den;
	uint64_t g;

	out->num = 1;
	out->den = 1;
	out->actual_mhz = rate * 1000U;
	out->mck_div = 0;
	out->ratio = 0;

	if (nrfx_i2s_prescalers_calc(&p, &pre) != 0) {
		return -EIO;
	}
	ratio = ratio_value(pre.ratio);
#if defined(I2S_MCKFREQ_FACTOR)
	{
		const uint32_t factor = (uint32_t)pre.mck_setup / 4096U;

		if (factor != 0) {
			div = I2S_MCKFREQ_FACTOR / factor;
		}
	}
#endif
#if NRF_I2S_HAS_CLKCONFIG
	if (pre.enable_bypass) {
		div = 1;
	}
#endif
	if (ratio == 0 || div == 0) {
		return -ENOTSUP;
	}

	/* fs_that = 32 MHz / (div * ratio); mau vao / mau ra = rate / fs_that */
	num = (uint64_t)rate * div * ratio;
	den = 32ULL * 1000 * 1000;
	g = gcd64(num, den);
	num /= g;
	den /= g;
	if (num * 100 > den * 105 || num * 105 < den * 100) {
		return -ERANGE;
	}

	out->num = (uint32_t)num;
	out->den = (uint32_t)den;
	out->actual_mhz = (uint32_t)(32ULL * 1000 * 1000 * 1000 / ((uint64_t)div * ratio));
	out->mck_div = div;
	out->ratio = ratio;
	return 0;
}
