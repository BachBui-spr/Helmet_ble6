/*
 * Helmet - chuc nang DIEN THOAI: VCP Volume Renderer + MCP client (play / pause / next).
 */

#ifndef FEATURES_PHONE_MEDIA_CTRL_H_
#define FEATURES_PHONE_MEDIA_CTRL_H_

#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

int media_ctrl_register(void); /* truoc bt_enable(): VCS vao GATT */
int media_ctrl_init(void);
void media_ctrl_connected(struct bt_conn *conn);
void media_ctrl_disconnected(void);
int media_ctrl_play_pause(void);
int media_ctrl_play(void);
int media_ctrl_pause(void);
int media_ctrl_next_track(void);
int media_ctrl_volume_up(void);
int media_ctrl_volume_down(void);
int media_ctrl_set_volume(uint8_t volume);
uint8_t media_ctrl_volume(void);
void media_ctrl_print_status(const struct shell *sh);

#endif /* FEATURES_PHONE_MEDIA_CTRL_H_ */
