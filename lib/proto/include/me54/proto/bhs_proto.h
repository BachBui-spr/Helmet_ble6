/*
 * Bike Hub Service (BHS): giao thuc mu <-> xe (apps/main hub_link.c <-> apps/bike bhs.c).
 * Mot nguon duy nhat cho UUID va dinh dang Join Info: sua o day la sua cho ca hai firmware.
 */

#ifndef ME54_PROTO_BHS_PROTO_H_
#define ME54_PROTO_BHS_PROTO_H_

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>

#define BHS_UUID_SVC_VAL  BT_UUID_128_ENCODE(0x0eed0101, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define BHS_UUID_JOIN_VAL BT_UUID_128_ENCODE(0x0eed0102, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define BHS_PROTO_VER     1
#define BHS_FLAG_PAIRING  BIT(0) /* trong Service Data quang ba: cua so ghep dang mo */
/* Join Info: [0] ver, [1..3] broadcast ID (LE24), [4] kieu dia chi, [5..10] dia chi bo phat
 * broadcast, [11] SID, [12] do dai ma (0 = mo), [13..28] ma broadcast (16 byte)
 */
#define BHS_JOIN_INFO_LEN 29

#endif /* ME54_PROTO_BHS_PROTO_H_ */
