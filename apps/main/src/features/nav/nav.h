/*
 * Helmet - chuc nang DINH VI (NAV): khoang cach toi xe, toi mu kia, va mu kia toi xe bang
 * Bluetooth Channel Sounding (CS IPT). Thiet ke: docs/HELMET_NAV_DINH_VI.md
 *   nav.c : bat / tat che do, quet + ket noi (ben ma nho hon / mu toi xe la central = ben do),
 *           bo do CS (initiator), trao doi so lieu voi mu kia qua HIS, log moi 2 s
 * Dinh vi LOAI TRU intercom va Auracast: bat Dinh vi thi ket thuc intercom, roi broadcast (tu vao
 * lai sau khi tat). Giao thuc: <me54/proto/nav_proto.h> (quang ba), IC_OP_NAV_* (his_proto.h).
 */

#ifndef FEATURES_NAV_NAV_H_
#define FEATURES_NAV_NAV_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

#include <me54/proto/nav_proto.h>

/* Service Data Dinh vi cua mu (NAV_ADV_LEN byte), app_bt.c dua vao quang ba khi dang Dinh vi */
extern uint8_t nav_adv_svc[NAV_ADV_LEN];

int nav_init(void);
void nav_handle(void); /* EVT_NAV, chi goi tu thread bo dieu khien */
void nav_toggle(void); /* giu Call 1 s */
bool nav_active(void); /* doc duoc tu moi thread */
void nav_shutdown(void); /* tat may: ngat cac lien ket Dinh vi */
int nav_shell(const struct shell *sh, size_t argc, char **argv);

/* app_bt.c (bt_rx): true = ket noi nay do nav.c quan ly (lien ket Dinh vi / ung vien mu ban) */
bool nav_conn_connected(struct bt_conn *conn, uint8_t err);
bool nav_conn_owned(const struct bt_conn *conn);
void nav_conn_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);
void nav_conn_disconnected(struct bt_conn *conn, uint8_t reason);

/* ic_service.c (bt_rx) */
void nav_on_his_discovered(struct bt_conn *conn, int err);
void nav_on_his_msg(struct bt_conn *conn, uint8_t op, uint8_t age100, uint16_t val);

#endif /* FEATURES_NAV_NAV_H_ */
