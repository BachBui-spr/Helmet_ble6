/*
 * Helmet (apps/main) - lop UNG DUNG: API duy nhat ma lop chuc nang (features/) duoc goi len.
 * Thiet ke: docs/HELMET_THIET_KE_CHUC_NANG_MOI.md. Cau truc ma nguon: README.md muc "Kien truc ma nguon"
 *
 * Lop chuc nang bao len ung dung bang app_post(EVT_*) (goi duoc tu moi thread / ISR), doc trang
 * thai thiet bi bang app_state(), doc / ghi cai dat chung qua helmet_cfg.
 */

#ifndef APP_APP_H_
#define APP_APP_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

/* ---------------- Trang thai thiet bi va su kien ---------------- */

enum helmet_state {
	HS_BOOT,
	HS_IDLE,      /* bat, dang quang ba, chua ket noi */
	HS_CONNECTED, /* da ket noi (nghe nhac hoac ranh) */
	HS_RINGING,   /* co cuoc goi den */
	HS_IN_CALL,   /* dang goi (hoac dang quay so / giu may) */
	HS_SHUTDOWN,  /* dang tat may */
};

enum helmet_evt_type {
	EVT_KEY_RAW,       /* a = phim, b = 1 nhan / 0 nha, v = thoi diem (ms) */
	EVT_KEY_TIMER,     /* a = phim (BUTTONS_COMBO_TIMER = hen gio to hop), v = the he hen gio */
	EVT_GESTURE,       /* a = phim, b = cu chi (lenh shell gia lap) */
	EVT_CONNECTED,     /* */
	EVT_DISCONNECTED,  /* a = ly do */
	EVT_SECURED,       /* lien ket da ma hoa */
	EVT_STREAM,        /* a = enum bt_audio_dir, b = 1 bat dau / 0 dung */
	EVT_CALL,          /* a = enum helmet_call (trang thai tong hop) */
	EVT_MIC_MUTE,      /* a = 1 tat / 0 bat */
	EVT_RING_CHECK,    /* 1 s sau khi co cuoc goi den: can chuong cuc bo khong */
	EVT_IDLE_TIMEOUT,  /* het gio tu tat */
	EVT_ADV_SLOWDOWN,  /* het 30 s quang ba nhanh */
	EVT_TEMP_ALARM,    /* a = 1 vuot nguong / 0 tro lai */
	EVT_LOW_BATTERY,   /* a = 1 canh bao / 2 phai tat */
	EVT_POWER_OFF_REQ, /* lenh shell */
	EVT_IC,            /* intercom: co viec moi trong hang doi rieng cua intercom.c */
	EVT_BC,            /* Auracast: co su kien moi cho broadcast_sink.c */
};

/* Goi duoc tu moi thread / ISR, khong bao gio cho */
void app_post(uint8_t type, uint8_t a, uint8_t b, uint32_t v);
enum helmet_state app_state(void);
const char *app_state_str(enum helmet_state s);
/* intercom.c: nhu cau quang ba doi (slave bat/tat co IC trong du lieu quang ba) */
void app_adv_refresh(void);
/* intercom.c: ket noi den khong phai mu ban -> giao cho luong dien thoai (nhan luon 1 ref) */
void app_phone_adopt(struct bt_conn *conn);
/* Co hoat dong (nghe, intercom...): hen gio tu tat tinh lai tu luc nay */
void power_activity(void);

/* ---------------- Cai dat luu qua lan tat (settings "helmet/...") ---------------- */

struct helmet_cfg {
	uint8_t volume;              /* 0..255, VCP */
	uint16_t autooff_min;        /* tu tat khi khong ket noi, 0 = khong bao gio */
	uint16_t temp_emis_permille; /* do phat xa x 1000 */
	int32_t temp_alarm_mc;       /* nguong canh bao nhiet, milli-do C */
};

extern struct helmet_cfg helmet_cfg;
void helmet_cfg_changed(void); /* hen ghi sau 10 s */
void helmet_cfg_save(void);    /* ghi ngay cac gia tri da doi */

#endif /* APP_APP_H_ */
