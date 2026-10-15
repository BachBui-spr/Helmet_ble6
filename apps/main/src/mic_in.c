/*
 * Helmet - thu 2 micro PDM (IM69D130) tren PDM20: CLK P1.11, DATA P1.09.
 *
 * - 16 kHz, 16 bit, stereo (2 micro tren cung 1 duong DATA, chon L/R bang chan SELECT).
 *   nRF54L15: 32 MHz / 25 = 1.28 MHz, ti le 80 -> dung 16 000 Hz (cung thach anh voi I2S).
 * - Khoi 10 ms = 160 mau x 2 kenh x 2 B = 640 B, 6 khoi trong kho.
 * - Micro chi chay khi co luong goi (khong co xung PDM -> micro ngu).
 * - Chi duoc goi tu thread giong noi (voice_tx.c).
 */

#include <zephyr/kernel.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include "helmet.h"

LOG_MODULE_REGISTER(mic_in, LOG_LEVEL_INF);

#define MIC_BLOCK_SIZE  (VOICE_FRAME * 2 * sizeof(int16_t))
#define MIC_BLOCK_COUNT 6

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(pdm20));
K_MEM_SLAB_DEFINE_STATIC(mic_slab, MIC_BLOCK_SIZE, MIC_BLOCK_COUNT, 4);

static bool ready;
static bool running;

int mic_in_init(void)
{
	ready = device_is_ready(dmic_dev);
	if (!ready) {
		LOG_ERR("PDM20 not ready - microphone disabled");
		return -ENODEV;
	}
	return 0;
}

bool mic_in_ready(void)
{
	return ready;
}

int mic_in_start(uint32_t *t0_cyc)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = VOICE_RATE,
		.pcm_width = 16,
		.block_size = MIC_BLOCK_SIZE,
		.mem_slab = &mic_slab,
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
	int err;

	if (!ready) {
		return -ENODEV;
	}

	err = dmic_configure(dmic_dev, &cfg);
	if (err) {
		LOG_ERR("dmic_configure failed (%d)", err);
		return err;
	}

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err) {
		LOG_ERR("DMIC START failed (%d)", err);
		return err;
	}
	*t0_cyc = k_cycle_get_32();
	running = true;
	LOG_INF("Microphones started: %u Hz stereo, PDM CLK P1.11 / DATA P1.09", VOICE_RATE);
	return 0;
}

int mic_in_read(int16_t **block, int32_t timeout_ms)
{
	void *buf = NULL;
	size_t size = 0;
	int err;

	if (!running) {
		return -EIO;
	}

	err = dmic_read(dmic_dev, 0, &buf, &size, timeout_ms);
	if (err) {
		return err;
	}
	if (size != MIC_BLOCK_SIZE) {
		k_mem_slab_free(&mic_slab, buf);
		return -EMSGSIZE;
	}
	*block = buf;
	return 0;
}

void mic_in_free(int16_t *block)
{
	if (block != NULL) {
		k_mem_slab_free(&mic_slab, (void *)block);
	}
}

void mic_in_stop(void)
{
	void *buf;
	size_t size;

	if (!running) {
		return;
	}
	(void)dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
	running = false;

	/* Tra lai cac khoi con nam trong hang doi cua driver */
	while (dmic_read(dmic_dev, 0, &buf, &size, 0) == 0) {
		k_mem_slab_free(&mic_slab, buf);
	}
	LOG_INF("Microphones stopped");
}
