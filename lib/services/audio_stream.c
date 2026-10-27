/*
 * Lop DICH VU - doc codec config LC3 cua BAP thanh audio_stream_cfg (xem audio_stream.h).
 */

#include <errno.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/hci_types.h>

#include <me54/services/audio_stream.h>

int audio_stream_cfg_parse(const struct bt_audio_codec_cfg *codec_cfg, enum bt_audio_dir dir,
			   struct audio_stream_cfg *out)
{
	enum bt_audio_location alloc;
	int ret;

	if (codec_cfg->id != BT_HCI_CODING_FORMAT_LC3) {
		return -ENOTSUP;
	}

	ret = bt_audio_codec_cfg_get_freq(codec_cfg);
	if (ret < 0) {
		return ret;
	}
	out->freq_hz = bt_audio_codec_cfg_freq_to_freq_hz(ret);

	ret = bt_audio_codec_cfg_get_frame_dur(codec_cfg);
	if (ret < 0) {
		return ret;
	}
	out->frame_dur_us = bt_audio_codec_cfg_frame_dur_to_frame_dur_us(ret);

	ret = bt_audio_codec_cfg_get_octets_per_frame(codec_cfg);
	if (ret <= 0) {
		return -EINVAL;
	}
	out->octets_per_frame = (uint16_t)ret;

	/* Khong co Audio Location -> mac dinh mono */
	ret = bt_audio_codec_cfg_get_chan_allocation(codec_cfg, &alloc, true);
	if (ret < 0) {
		return ret;
	}
	out->chan_alloc = alloc;
	out->chan_cnt = bt_audio_get_chan_count(alloc);

	ret = bt_audio_codec_cfg_get_frame_blocks_per_sdu(codec_cfg, true);
	if (ret != 1) {
		return -ENOTSUP; /* PACS chi cho phep 1 khung/SDU */
	}

	if (dir == BT_AUDIO_DIR_SOURCE) {
		if (out->chan_cnt != 1 || out->frame_dur_us != 10000 ||
		    (out->freq_hz != 16000 && out->freq_hz != 32000) ||
		    out->octets_per_frame < 30 || out->octets_per_frame > 80) {
			return -ENOTSUP;
		}
		/* 32 kHz: PAC chi khai 16 kHz nhung van nhan (dien thoai con PAC cu); le_audio.c canh bao */
		return 0;
	}

	if (out->chan_cnt > 2 || out->octets_per_frame * out->chan_cnt > AUDIO_STREAM_MAX_SDU) {
		return -ENOTSUP;
	}

	return 0;
}

