/*
 * Helmet - chuc nang DIEN THOAI: BAP Unicast Server, 2 ASE sink (nghe) + 1 ASE source (micro).
 */

#ifndef FEATURES_PHONE_LE_AUDIO_H_
#define FEATURES_PHONE_LE_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/shell/shell.h>

/* Chieu nhan: nghe nhac + cuoc goi (Conversational) + chuong (Ringtone) */
#define HELMET_SINK_CONTEXTS                                                                       \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL |                \
	 BT_AUDIO_CONTEXT_TYPE_MEDIA | BT_AUDIO_CONTEXT_TYPE_GAME |                                \
	 BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL | BT_AUDIO_CONTEXT_TYPE_SOUND_EFFECTS |               \
	 BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS | BT_AUDIO_CONTEXT_TYPE_RINGTONE |                    \
	 BT_AUDIO_CONTEXT_TYPE_ALERTS)

/* Chieu gui (micro): chi cuoc goi */
#define HELMET_SOURCE_CONTEXTS                                                                     \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)

#define HELMET_SINK_LOCATION   (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)
#define HELMET_SOURCE_LOCATION BT_AUDIO_LOCATION_FRONT_LEFT

#define HELMET_MAX_STREAMS CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT

struct helmet_rx_flags {
	uint32_t ok;    /* du lieu tot */
	uint32_t crc;   /* BT_ISO_FLAGS_ERROR: nhan duoc nhung co the hong */
	uint32_t lost;  /* BT_ISO_FLAGS_LOST: khong nhan duoc gi */
	uint32_t empty; /* hop le nhung do dai 0 */
};

int le_audio_register(void); /* truoc bt_enable(): PACS + ASCS vao GATT */
int le_audio_init(void);
bool le_audio_is_streaming(void);        /* co it nhat 1 ASE sink dang phat */
bool le_audio_source_streaming(void);    /* ASE source (micro) dang phat */
void le_audio_set_source_available(bool available);
void le_audio_print_status(const struct shell *sh);
const char *le_audio_phy_str(uint8_t phy);
void le_audio_set_qos_pref(uint8_t phy, uint8_t rtn, uint16_t latency_ms);
void le_audio_get_rx_flags(struct helmet_rx_flags *out);
void le_audio_print_link_quality(const struct shell *sh);

#endif /* FEATURES_PHONE_LE_AUDIO_H_ */
