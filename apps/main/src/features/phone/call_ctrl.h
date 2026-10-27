/*
 * Helmet - chuc nang DIEN THOAI: TBS client (nghe / tu choi / ket thuc cuoc goi) + MICP
 * Microphone Device.
 */

#ifndef FEATURES_PHONE_CALL_CTRL_H_
#define FEATURES_PHONE_CALL_CTRL_H_

#include <stdbool.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

enum helmet_call { CALL_NONE, CALL_INCOMING, CALL_OUTGOING, CALL_ACTIVE, CALL_HELD };

int call_ctrl_register(void); /* truoc bt_enable(): MICS vao GATT */
int call_ctrl_init(void);
void call_ctrl_connected(struct bt_conn *conn); /* bat dau do GTBS */
void call_ctrl_disconnected(void);
int call_ctrl_accept(void);
int call_ctrl_reject(void);
int call_ctrl_hangup(void);
int call_ctrl_mic_mute(bool mute);
void call_ctrl_mic_unavailable(bool unavailable);
enum helmet_call call_ctrl_state(void);
const char *call_ctrl_state_str(enum helmet_call c);
void call_ctrl_print_status(const struct shell *sh);

#endif /* FEATURES_PHONE_CALL_CTRL_H_ */
