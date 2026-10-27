/*
 * Lop CORE - khung LC3 im lang (xem lc3_silence.h).
 */

#include <errno.h>
#include <string.h>

#include <lc3.h>

#include <me54/core/lc3_silence.h>

#define MAX_SAMPLES (48000 / 100) /* 10 ms o 48 kHz */

int lc3_silence_frame(uint32_t frame_us, uint32_t freq_hz, uint16_t octets, uint8_t *out)
{
	static lc3_encoder_mem_48k_t mem;
	static int16_t pcm[MAX_SAMPLES];
	lc3_encoder_t enc;

	if (freq_hz > 48000 || frame_us > 10000) {
		return -EINVAL;
	}
	enc = lc3_setup_encoder((int)frame_us, (int)freq_hz, 0, &mem);
	if (enc == NULL) {
		return -EIO;
	}
	memset(pcm, 0, sizeof(pcm));
	return lc3_encode(enc, LC3_PCM_FORMAT_S16, pcm, 1, octets, out) == 0 ? 0 : -EIO;
}
