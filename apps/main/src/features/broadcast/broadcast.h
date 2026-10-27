/*
 * Helmet - chuc nang BROADCAST (Auracast):
 *   broadcast_sink.c : BAP Broadcast Sink + Scan Delegator (BASS), nghe chung nhac
 *   hub_link.c       : lien ket tam toi Bike hub (apps/bike): lay broadcast ID / dia chi / ma qua BHS
 * Giao thuc voi xe: <me54/proto/bhs_proto.h>.
 */

#ifndef FEATURES_BROADCAST_BROADCAST_H_
#define FEATURES_BROADCAST_BROADCAST_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

#include <me54/proto/bhs_proto.h>

enum bc_mode {
	BC_OFF,         /* khong nghe broadcast */
	BC_SCAN,        /* tu quet tim broadcast (khong co dien thoai chi duong) */
	BC_HUB_LINK,    /* dang ket noi Bike hub de lay thong tin vao xe (ghep bang nut) */
	BC_PA_WAIT,     /* cho dong bo PA (PAST tu dien thoai, hoac tu tao) */
	BC_PA_SYNCED,   /* da dong bo PA: cho BASE, BIGInfo, ma, yeu cau BIS */
	BC_BIG_SYNCING, /* da xin dong bo BIG, cho controller */
	BC_STREAMING,   /* dang nhan BIS */
};

int bcast_register(void); /* truoc bt_enable(): BASS vao GATT */
int bcast_init(void);     /* sau settings_load() */
void bcast_handle(void);  /* EVT_BC, chi goi tu thread bo dieu khien */
void bcast_toggle(void);  /* nut: tu tim broadcast gan nhat, hoac roi broadcast dang nghe */
void bcast_leave(void);   /* roi broadcast (intercom, Dinh vi, tat may); thread bo dieu khien */
/* Het Dinh vi: vao lai broadcast cua hub da ghep sau vai giay (neu chua roi bang nut) */
void bcast_rejoin_soon(void);
bool bcast_active(void);  /* khac BC_OFF (doc duoc tu moi thread) */
enum bc_mode bcast_mode(void);
const char *bcast_mode_str(enum bc_mode m);
/* le_audio.c (bt_rx): luong sink unicast bat dau (true) / het (false): unicast chiem loa */
void bcast_audio_hold(bool hold);
int bcast_shell(const struct shell *sh, size_t argc, char **argv);
void bcast_print(const struct shell *sh);
void bcast_log_status(void); /* 1 dong heartbeat khi khac BC_OFF */
/* hub_link.c (bt_rx): ket qua doc Join Info. err != 0: that bai */
void bcast_hub_result(int err, const uint8_t *info, uint16_t len);

/* hub_link.c */
int hub_link_start(const bt_addr_le_t *addr);
void hub_link_cancel(void);
bool hub_link_owned(const struct bt_conn *conn);
void hub_link_connected(struct bt_conn *conn, uint8_t err);
void hub_link_secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err);
void hub_link_disconnected(struct bt_conn *conn, uint8_t reason);
/* Sau ket qua -EAGAIN: dia chi hub co khoa ghep doi cu (de xoa). false: khong co */
bool hub_link_stale_peer(bt_addr_le_t *out);

#endif /* FEATURES_BROADCAST_BROADCAST_H_ */
