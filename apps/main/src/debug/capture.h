/*
 * Helmet - CHAN DOAN: chep SDU LC3 nhan duoc ve PC qua RTT kenh 1 (scripts/helmet_audio.py).
 */

#ifndef DEBUG_CAPTURE_H_
#define DEBUG_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <me54/services/audio_stream.h>

int capture_init(void);
void capture_set_enabled(bool on);
void capture_stream_start(uint8_t idx, const struct audio_stream_cfg *cfg);
void capture_stream_stop(uint8_t idx);
void capture_sdu(uint8_t idx, uint8_t iso_flags, uint16_t iso_seq, uint32_t ts,
		 const uint8_t *data, uint16_t len);
void capture_print_status(const struct shell *sh);

#endif /* DEBUG_CAPTURE_H_ */
