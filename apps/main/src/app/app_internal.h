/*
 * Helmet (apps/main) - khai bao dung chung giua cac file cua lop ung dung (app/). Lop chuc nang
 * khong include file nay (chi app/app.h).
 *
 *   app_ctrl.c    : hang su kien, may trang thai thiet bi, cuoc goi, heartbeat, main()
 *   app_bt.c      : quang ba LE Audio, ket noi / bao mat, giao ket noi cho luong dien thoai
 *   app_gesture.c : cu chi nut -> hanh dong
 *   app_shell.c   : lenh shell "helmet ..."
 *   app_config.c  : cai dat luu qua lan tat (helmet_cfg)
 *   power.c       : quy trinh tat may, tu tat, quang ba cham, canh bao pin
 */

#ifndef APP_APP_INTERNAL_H_
#define APP_APP_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/tmap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

#include "app/app.h"

/* TMAP: nghe nhac unicast (UMR), goi dien (CT), nghe broadcast Auracast (BMR) */
#define HELMET_TMAP_ROLES (BT_TMAP_ROLE_UMR | BT_TMAP_ROLE_CT | BT_TMAP_ROLE_BMR)

struct helmet_evt {
	uint8_t type;
	uint8_t a;
	uint8_t b;
	uint8_t pad;
	uint32_t v;
};

/* ---------------- app_ctrl.c ---------------- */
void app_set_state(enum helmet_state s);

/* ---------------- app_gesture.c (thread bo dieu khien) ---------------- */
void app_gesture(uint8_t key, uint8_t gesture);

/* ---------------- app_bt.c ---------------- */
int app_bt_adv_create(void);
void app_bt_register_auth(void);
/* Ket noi dien thoai dang dung (khong lay ref) hoac NULL */
struct bt_conn *app_bt_phone(void);
bt_security_t app_bt_sec_level(void);
bool app_bt_adv_slow(void);
/* power.c: ngat ket noi (cho toi 1 s) + dung quang ba */
void app_bt_shutdown(void);
void app_adv_set_slow(bool slow);

/* ---------------- app_config.c ---------------- */
void helmet_cfg_loaded(void); /* sau settings_load(): chot gia tri da luu */

/* ---------------- power.c ---------------- */
enum power_off_reason { OFF_BUTTON, OFF_IDLE, OFF_LOW_BATTERY, OFF_SHELL };

/* Goi dau tien trong main(). Tra ve true neu may duoc bat bang nut Power (nut co the
 * van dang giu). Co the KHONG tra ve: thuc day nham -> tat lai ngay.
 */
bool power_boot_check(void);
FUNC_NORETURN void power_off(enum power_off_reason why);
void power_state_changed(enum helmet_state s);
void power_autooff_changed(void);
int power_vdd_mv(void);
void power_print_status(const struct shell *sh);

#endif /* APP_APP_INTERNAL_H_ */
