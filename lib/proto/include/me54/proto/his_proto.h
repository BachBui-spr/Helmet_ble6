/*
 * Helmet Intercom Service (HIS): giao thuc mu <-> mu (docs/HELMET_THIET_KE_INTERCOM.md muc 6.2).
 */

#ifndef ME54_PROTO_HIS_PROTO_H_
#define ME54_PROTO_HIS_PROTO_H_

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>

#define IC_UUID_SVC_VAL BT_UUID_128_ENCODE(0x0eed0001, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define IC_UUID_CP_VAL  BT_UUID_128_ENCODE(0x0eed0003, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)
#define IC_UUID_EVT_VAL BT_UUID_128_ENCODE(0x0eed0004, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)

#define IC_PROTO_VER      1
#define IC_ADV_FLAG_SLAVE BIT(0) /* trong Service Data quang ba: dang cho master ket noi */
#define IC_MSG_LEN        4      /* [op][session][arg][arg2] */

enum ic_op {
	IC_OP_HELLO = 0x01, /* arg = phien ban giao thuc */
	IC_OP_END = 0x14,   /* arg = enum ic_reason */
};

enum ic_reason {
	IC_REASON_USER = 0x00,
	IC_REASON_POWER_OFF = 0x02,
	IC_REASON_NOT_ALLOWED = 0x06,
	IC_REASON_VERSION = 0x07,
};

#endif /* ME54_PROTO_HIS_PROTO_H_ */
