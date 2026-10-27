/*
 * Lop HAL: 4 nut cua board (gpio-keys, chong doi 30 ms trong devicetree) -> su kien tho.
 * Nhan dang cu chi (ngan / dup / giu / to hop) la viec cua lop tren (services/buttons.c o mu,
 * app/ui.c o xe).
 */

#ifndef ME54_HAL_KEYS_H_
#define ME54_HAL_KEYS_H_

#include <stdbool.h>
#include <stdint.h>

enum key_id { KEY_VOL_UP, KEY_VOL_DOWN, KEY_CALL, KEY_POWER, KEY_COUNT };

/* Goi tu thread input (khong duoc cho lau). t_ms = k_uptime_get_32() luc co su kien */
typedef void (*keys_cb_t)(uint8_t key, bool pressed, uint32_t t_ms);

/* Dang ky noi nhan su kien; goi som nhat co the (truoc khi doc nut luc khoi dong) */
void keys_init(keys_cb_t cb);

#endif /* ME54_HAL_KEYS_H_ */
