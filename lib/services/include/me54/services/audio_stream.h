/*
 * Lop DICH VU: cau hinh mot luong am thanh LC3 (ASE unicast, BIS broadcast, CIS intercom) va
 * cach doc no tu codec config cua BAP.
 */

#ifndef ME54_SERVICES_AUDIO_STREAM_H_
#define ME54_SERVICES_AUDIO_STREAM_H_

#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>

/* SDU lon nhat chieu nhan: 2 kenh x 155 octet/khung x 1 khung/SDU */
#define AUDIO_STREAM_MAX_SDU 320

/* Cau hinh codec cua mot luong da duoc ben kia chon */
struct audio_stream_cfg {
	uint32_t freq_hz;
	uint32_t frame_dur_us;
	uint16_t octets_per_frame;
	uint8_t chan_cnt;
	uint32_t chan_alloc; /* enum bt_audio_location, 0 = mono */
};

/* Doc codec config LC3, kiem tra mu ho tro duoc:
 *   SINK (nghe): 1-2 kenh, SDU <= AUDIO_STREAM_MAX_SDU;
 *   SOURCE (micro): 1 kenh, 10 ms, 16 / 32 kHz, 30-80 octet.
 * Tra ve 0, -ENOTSUP / -EINVAL hoac loi cua bt_audio_codec_cfg_get_*().
 */
int audio_stream_cfg_parse(const struct bt_audio_codec_cfg *codec_cfg, enum bt_audio_dir dir,
			   struct audio_stream_cfg *out);

static inline int audio_stream_cfg_parse_sink(const struct bt_audio_codec_cfg *codec_cfg,
					      struct audio_stream_cfg *out)
{
	return audio_stream_cfg_parse(codec_cfg, BT_AUDIO_DIR_SINK, out);
}

#endif /* ME54_SERVICES_AUDIO_STREAM_H_ */
