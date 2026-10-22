/*
 * Bike hub (apps/bike) - khai bao dung chung.
 * Thiet ke: docs/BIKE_HUB_THIET_KE.md
 *
 *   main.c         : khoi dong BT, quang ba cho dien thoai, ket noi / bao mat, cai dat, shell
 *   phone_sink.c   : BAP Unicast Server (chi chieu nhan): dien thoai gui nhac LC3 qua CIS
 *   bcast_source.c : BAP Broadcast Source (Auracast): phat lai nguyen khung LC3 qua 2 BIS
 *                    (trai / phai) cho moi mu o gan; hang doi bu troi clock, khung im lang
 *   bhs.c          : Bike Hub Service + cua so ghep mu 60 s: mu doc broadcast ID / dia chi / ma
 *   hub_ui.c       : 4 nut, am bao qua loa, bat / tat nguon (board giong het mu)
 *   nav_hub.c      : che do Dinh vi - xe chi phan xa Channel Sounding, mu tu do va tinh
 *
 * Hub khong giai ma / ma hoa lai am thanh: khung LC3 tu dien thoai duoc chuyen thang sang BIS.
 * Vi vay cau hinh LC3 cua BIG phai trung voi cau hinh dien thoai chon (48 kHz, 10 ms, cung
 * so octet moi khung); bcast_source.c tu tao lai BIG khi dien thoai doi so octet.
 */

#ifndef BIKE_H_
#define BIKE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
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

/* ---------------- phone_sink.c ---------------- */

int phone_sink_register(void); /* TRUOC bt_enable(): PACS + ASCS vao GATT */
int phone_sink_init(void);     /* sau bt_enable() + settings_load() */
bool phone_sink_streaming(void);
void phone_sink_print(const struct shell *sh);

/* ---------------- bcast_source.c ---------------- */

int bcast_source_init(void); /* sau bt_enable() + settings_load(): tao va bat dau BIG */

/* phone_sink.c (bt_rx): cap khung LC3 cua CUNG mot khoang 10 ms cho BIS trai va phai (luon gui
 * cung nhau de hai BIS khong lech nhau). NULL: kenh do bi mat goi tu dien thoai (hub lap lai khung
 * truoc mot lan, sau do phat im lang). len = so octet moi khung.
 */
void bcast_source_push_pair(const uint8_t *left, const uint8_t *right, uint16_t len);

/* phone_sink.c: luong tu dien thoai bat dau (octets = so octet moi kenh) / ket thuc */
void bcast_source_phone_started(uint16_t octets);
void bcast_source_phone_stopped(void);
/* Che do Dinh vi: tam dung / phat lai BIG (nhuong radio cho Channel Sounding) */
void bcast_source_pause(bool pause);

enum bike_code_mode {
	BIKE_CODE_RANDOM, /* mac dinh: 16 byte ngau nhien, mu nhan qua BHS (khong ai phai go) */
	BIKE_CODE_TEXT,   /* chuoi tu dat (thu nghiem) */
	BIKE_CODE_OPEN,   /* khong ma hoa */
};
/* Luu + tao lai BIG. Doi ma thi cac mu da ghep phai ghep lai (giu nut "vao xe"). */
int bcast_source_set_code(enum bike_code_mode mode, const char *text);
int bcast_source_set_rtn(uint8_t rtn);       /* so lan phat lai moi PDU cua BIS; luu + tao lai */
void bcast_source_print(const struct shell *sh);
void bcast_source_log_status(void);
/* Goi bt_rx (BHS): dien BHS_JOIN_INFO_LEN byte thong tin vao xe */
void bcast_source_join_info(uint8_t *out);

/* ---------------- bhs.c: Bike Hub Service (cung UUID voi apps/main/src/hub_link.c) ---------------- */

#define BHS_UUID_SVC_VAL  BT_UUID_128_ENCODE(0x0eed0101, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define BHS_UUID_JOIN_VAL BT_UUID_128_ENCODE(0x0eed0102, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define BHS_PROTO_VER     1
#define BHS_FLAG_PAIRING  BIT(0) /* trong Service Data quang ba: cua so ghep dang mo */
/* Join Info: [0] ver, [1..3] broadcast ID (LE24), [4] kieu dia chi, [5..10] dia chi bo phat
 * broadcast, [11] SID, [12] do dai ma (0 = mo), [13..28] ma broadcast (16 byte)
 */
#define BHS_JOIN_INFO_LEN 29

int bhs_init(void);
void bhs_pair_start(void);  /* mo cua so ghep mu 60 s (goi lai: gia han) */
void bhs_pair_cancel(void); /* dong ngay */
bool bhs_pair_active(void);
bool bhs_is_helmet(const struct bt_conn *conn); /* ket noi nay la mu dang ghep */
void bhs_conn_disconnected(struct bt_conn *conn);
void bhs_print(const struct shell *sh);

/* ---------------- hub_ui.c ---------------- */

enum hub_tone {
	HUB_TONE_POWER_ON,
	HUB_TONE_POWER_OFF,
	HUB_TONE_CONNECTED,
	HUB_TONE_DISCONNECTED,
	HUB_TONE_PAIR_SEARCH, /* lap: cua so ghep mu dang mo */
	HUB_TONE_HELMET_JOINED,
	HUB_TONE_PAIR_END, /* dong cua so, co it nhat 1 mu */
	HUB_TONE_FAIL,     /* dong cua so, khong co mu nao */
	HUB_TONE_STATUS_OK,
	HUB_TONE_STATUS_NO_LINK,
	HUB_TONE_UNPAIR,
	HUB_TONE_NAV_ON,  /* bat che do Dinh vi */
	HUB_TONE_NAV_OFF,
	HUB_TONE_COUNT,
};

enum hub_gesture {
	HUB_G_STATUS,    /* Power ngan */
	HUB_G_POWER_OFF, /* Power giu 3 s */
	HUB_G_PAIR,      /* Power ngan -> giu Vol+ 2 s */
	HUB_G_UNPAIR,    /* Vol+ va Vol- giu 5 s */
	HUB_G_CALL,      /* Call ngan: bat / tat che do Dinh vi */
};

int hub_ui_init(void);
bool hub_power_boot_check(void); /* dau main(): thuc tu System OFF thi phai giu Power 2 s */
FUNC_NORETURN void hub_power_off(void);
void hub_tone(enum hub_tone t);
void hub_tone_stop(void);

/* ---------------- main.c ---------------- */

void hub_gesture(enum hub_gesture g); /* goi tu thread hub_ui */
void hub_on_helmet(struct bt_conn *conn); /* BHS: ket noi nay la mu (khong phai dien thoai) */
void hub_bt_shutdown(void);             /* tat may: ngat moi ket noi, dung quang ba */

/* ---------------- nav_hub.c: che do Dinh vi (docs/HELMET_NAV_DINH_VI.md) ---------------- */

int nav_hub_init(void); /* sau bt_enable() + settings_load() */
void nav_hub_set(bool on);
void nav_hub_toggle(void);
void nav_hub_log_status(void);
int nav_hub_shell(const struct shell *sh, size_t argc, char **argv);

#endif /* BIKE_H_ */
