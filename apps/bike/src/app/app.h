/*
 * Bike hub (apps/bike) - lop UNG DUNG: API ma lop chuc nang (features/) duoc goi len.
 * Thiet ke: docs/BIKE_HUB_THIET_KE.md. Cau truc ma nguon: README.md muc "Kien truc ma nguon"
 *
 *   app/main.c                    : khoi dong BT, quang ba cho dien thoai, ket noi / bao mat, shell
 *   app/ui.c                      : 4 nut (cu chi rieng cua hub), am bao, bat / tat nguon
 *   features/phone/phone_sink.c   : BAP Unicast Server (chi chieu nhan): dien thoai gui nhac LC3
 *   features/broadcast/bcast_source.c : BAP Broadcast Source (Auracast) cho moi mu o gan
 *   features/broadcast/bhs.c      : Bike Hub Service + cua so ghep mu 60 s
 *   features/nav/nav_hub.c        : che do Dinh vi - xe chi phan xa Channel Sounding, mu tu do
 *
 * Hub khong giai ma / ma hoa lai am thanh: khung LC3 tu dien thoai duoc chuyen thang sang BIS.
 */

#ifndef APP_APP_H_
#define APP_APP_H_

#include <stdbool.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/toolchain.h>

/* ---------------- ui.c ---------------- */

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

#endif /* APP_APP_H_ */
