/*
 * Helmet - chuc nang INTERCOM mu - mu: chon vai tro bang nut, ket noi, ghep doi, noi lai.
 *   intercom.c   : may trang thai
 *   ic_service.c : Helmet Intercom Service (GATT): server o moi mu, client o master
 *   ic_audio.c   : BAP Unicast Client o master, LC3 16 kHz hai chieu toi slave
 * Giao thuc: <me54/proto/his_proto.h>.
 */

#ifndef FEATURES_INTERCOM_INTERCOM_H_
#define FEATURES_INTERCOM_INTERCOM_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

#include <me54/proto/his_proto.h>

enum ic_role { IC_ROLE_NONE, IC_ROLE_MASTER, IC_ROLE_SLAVE };

enum ic_mode {
	ICM_NORMAL,  /* khong co vai tro, mu chay nhu binh thuong */
	ICM_SEARCH,  /* master: dang quet tim slave */
	ICM_WAIT,    /* slave: dang quang ba co IC, cho master */
	ICM_LINKING, /* master: dang ket noi / ma hoa / bat tay HIS */
	ICM_LINKED,  /* da ket noi voi mu ban */
	ICM_ENDING,  /* dang ket thuc (gui END, cho ngat) */
};

int intercom_init(void);
void intercom_handle(void); /* EVT_IC, chi goi tu thread bo dieu khien */
void intercom_select(enum ic_role role);
void intercom_stop(void);
enum ic_mode intercom_mode(void);
const char *intercom_mode_str(enum ic_mode m);
void intercom_shutdown(void); /* power_off: gui END, ngat lien ket mu-mu */
int intercom_shell(const struct shell *sh, size_t argc, char **argv);
void intercom_log_status(void); /* 1 dong heartbeat khi khac ICM_NORMAL */

/* Goi tu callback Bluetooth (app_bt.c). true = ket noi nay do intercom quan ly */
bool intercom_conn_connected(struct bt_conn *conn, uint8_t err);
bool intercom_conn_owned(const struct bt_conn *conn);
void intercom_conn_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);
void intercom_conn_disconnected(struct bt_conn *conn, uint8_t reason);
void intercom_conn_release(struct bt_conn *conn); /* bo khoi bang quan ly (giao dien thoai) */
bool intercom_adv_ic(void); /* slave dang cho: quang ba them co IC, toc do nhanh */

/* Goi tu ic_service.c / ic_audio.c (bt_rx) */
void intercom_on_msg(struct bt_conn *conn, uint8_t op, uint8_t arg);
void intercom_on_discovered(struct bt_conn *conn, int err);
void intercom_on_audio(uint8_t ev, uint8_t arg);

/* ic_audio.c (master) - moi ham tru init chi goi tu thread bo dieu khien */
int ic_audio_init(void);
int ic_audio_start(struct bt_conn *conn);
void ic_audio_on_event(uint8_t ev, uint8_t arg);
void ic_audio_conn_lost(void);
bool ic_audio_streaming(void);
const char *ic_audio_state_str(void);
void ic_audio_print(const struct shell *sh);

/* ic_service.c */
int ic_svc_discover(struct bt_conn *conn); /* master: tim HIS + dang ky Event */
int ic_svc_send(struct bt_conn *conn, bool to_slave, uint8_t op, uint8_t arg);
bool ic_svc_busy(void); /* con lenh ghi / indicate chua xong */

#endif /* FEATURES_INTERCOM_INTERCOM_H_ */
