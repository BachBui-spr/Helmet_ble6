/*
 * Bike hub - chuc nang DINH VI (NAV): xe chi PHAN XA Channel Sounding, moi mu tu do va tu tinh
 * khoang cach toi xe. Thiet ke: docs/HELMET_NAV_DINH_VI.md
 * Bat Dinh vi thi xe tam dung Auracast (bcast_source_pause) de nhuong radio cho CS.
 * Giao thuc: <me54/proto/nav_proto.h> (quang ba).
 */

#ifndef FEATURES_NAV_NAV_HUB_H_
#define FEATURES_NAV_NAV_HUB_H_

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/shell/shell.h>

int nav_hub_init(void); /* sau bt_enable() + settings_load() */
void nav_hub_set(bool on);
void nav_hub_toggle(void); /* Call ngan tren hub */
void nav_hub_log_status(void);
int nav_hub_shell(const struct shell *sh, size_t argc, char **argv);

#endif /* FEATURES_NAV_NAV_HUB_H_ */
