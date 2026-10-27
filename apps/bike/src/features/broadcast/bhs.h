/*
 * Bike hub - chuc nang BROADCAST: Bike Hub Service + cua so ghep mu 60 s (mu doc broadcast ID /
 * dia chi / ma). Giao thuc chung voi mu: <me54/proto/bhs_proto.h>.
 */

#ifndef FEATURES_BROADCAST_BHS_H_
#define FEATURES_BROADCAST_BHS_H_

#include <stdbool.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

#include <me54/proto/bhs_proto.h>

int bhs_init(void);
void bhs_pair_start(void);  /* mo cua so ghep mu 60 s (goi lai: gia han) */
void bhs_pair_cancel(void); /* dong ngay */
bool bhs_pair_active(void);
bool bhs_is_helmet(const struct bt_conn *conn); /* ket noi nay la mu dang ghep */
void bhs_conn_disconnected(struct bt_conn *conn);
void bhs_print(const struct shell *sh);

#endif /* FEATURES_BROADCAST_BHS_H_ */
