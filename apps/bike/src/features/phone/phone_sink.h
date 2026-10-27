/*
 * Bike hub - chuc nang DIEN THOAI: BAP Unicast Server, chi chieu NHAN (hub khong co micro).
 * Moi SDU nhan qua CIS duoc tach thanh cap khung trai / phai va chuyen sang bcast_source.c.
 */

#ifndef FEATURES_PHONE_PHONE_SINK_H_
#define FEATURES_PHONE_PHONE_SINK_H_

#include <stdbool.h>

#include <zephyr/shell/shell.h>

int phone_sink_register(void); /* TRUOC bt_enable(): PACS + ASCS vao GATT */
int phone_sink_init(void);     /* sau bt_enable() + settings_load() */
bool phone_sink_streaming(void);
void phone_sink_print(const struct shell *sh);
void phone_sink_log_status(void); /* heartbeat: SDU CIS nhan / mat trong 10 s vua qua */

#endif /* FEATURES_PHONE_PHONE_SINK_H_ */
