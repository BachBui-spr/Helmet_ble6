/*
 * Bike hub - chuc nang BROADCAST: BAP Broadcast Source (Auracast) phat lai nguyen khung LC3 cho
 * moi mu o gan (BIG 20 ms 1 BIS stereo hoac BIG 10 ms 2 BIS trai / phai).
 *
 * Cau hinh LC3 cua BIG phai trung voi cau hinh dien thoai chon (48 kHz, 10 ms, cung so octet
 * moi khung): phone_sink.c (chuc nang dien thoai) day khung vao day qua bcast_source_push_pair().
 */

#ifndef FEATURES_BROADCAST_BCAST_SOURCE_H_
#define FEATURES_BROADCAST_BCAST_SOURCE_H_

#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/* Cau hinh LC3 hub nhan tu dien thoai va phat lai (BAP 48_4 mac dinh) */
#define BIKE_FREQ_HZ        48000
#define BIKE_FRAME_US       10000
#define BIKE_OCTETS_MIN     100 /* 48_2 */
#define BIKE_OCTETS_MAX     155 /* 48_6 */
#define BIKE_OCTETS_DEFAULT 120 /* 48_4: dien thoai thuong chon cho nhac */

/* Kenh: bit 0 = trai (FL, BIS 1), bit 1 = phai (FR, BIS 2) */
#define BIKE_CH_L BIT(0)
#define BIKE_CH_R BIT(1)

int bcast_source_init(void); /* sau bt_enable() + settings_load(): tao va bat dau BIG */

/* phone_sink.c (bt_rx): cap khung LC3 cua CUNG mot khoang 10 ms cho BIS trai va phai (luon gui
 * cung nhau de hai BIS khong lech nhau). NULL: kenh do bi mat goi tu dien thoai (hub lap lai khung
 * truoc mot lan, sau do phat im lang). len = so octet moi khung.
 */
void bcast_source_push_pair(const uint8_t *left, const uint8_t *right, uint16_t len);

/* phone_sink.c: luong tu dien thoai bat dau (octets = so octet moi kenh) / ket thuc */
void bcast_source_phone_started(uint16_t octets);
void bcast_source_phone_stopped(void);

enum bike_code_mode {
	BIKE_CODE_RANDOM, /* mac dinh: 16 byte ngau nhien, mu nhan qua BHS (khong ai phai go) */
	BIKE_CODE_TEXT,   /* chuoi tu dat (thu nghiem) */
	BIKE_CODE_OPEN,   /* khong ma hoa */
};
/* Luu + tao lai BIG. Doi ma thi cac mu da ghep phai ghep lai (giu nut "vao xe"). */
int bcast_source_set_code(enum bike_code_mode mode, const char *text);
int bcast_source_set_rtn(uint8_t rtn);       /* so lan phat lai moi PDU cua BIS; luu + tao lai */
/* Chu ky BIG: 20 = 1 BIS stereo, BN 2 (mac dinh); 10 = 2 BIS trai / phai. Luu + tao lai */
int bcast_source_set_isoint(uint8_t ms);
uint8_t bcast_source_iso_ms(void); /* chu ky BIG dang chay (10 / 20 ms) */
void bcast_source_print(const struct shell *sh);
void bcast_source_log_status(void);
/* Goi bt_rx (BHS): dien BHS_JOIN_INFO_LEN byte thong tin vao xe */
void bcast_source_join_info(uint8_t *out);

#endif /* FEATURES_BROADCAST_BCAST_SOURCE_H_ */
