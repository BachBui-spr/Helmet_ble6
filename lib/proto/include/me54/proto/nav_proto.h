/*
 * Che do Dinh vi (NAV) - phan dung chung mu (apps/main) va xe (apps/bike).
 * Thiet ke: docs/HELMET_NAV_DINH_VI.md
 *
 * Chi co dinh dang QUANG BA Dinh vi: thiet bi dang o che do Dinh vi them Service Data 128 bit
 *   {UUID 0eed0201-..., ver, loai (xe / mu), ma 4 byte (LE)}
 * vao du lieu quang ba co the ket noi. Mu quet thay xe thi ket noi toi xe; hai mu thay nhau thi
 * mu co MA NHO HON ket noi. Ben ket noi (central) la ben do (CS initiator) va tu tinh khoang cach;
 * ben kia chi phan xa. Xe khong co dich vu GATT nao cho Dinh vi.
 * So lieu giua hai mu di qua Helmet Intercom Service (HIS) co san: IC_OP_NAV_* trong his_proto.h.
 */

#ifndef ME54_PROTO_NAV_PROTO_H_
#define ME54_PROTO_NAV_PROTO_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

/* Cung ho 0eedxxxx voi HIS (0eed0001, mu-mu) va BHS (0eed0101, ghep mu-hub) */
#define NAV_UUID_SVC_VAL BT_UUID_128_ENCODE(0x0eed0201, 0x080c, 0x4225, 0xa3d0, 0xf91ce86a80c2)

/* Phien ban 2: thiet ke don gian (ban 1 co giao thuc xe-mu rieng, da bo) */
#define NAV_PROTO_VER 2

enum nav_dev_type {
	NAV_TYPE_BIKE = 0,
	NAV_TYPE_HELMET = 1,
};

/* Service Data 128: [0..15] UUID, [16] ver, [17] loai, [18..21] ma (LE) */
#define NAV_ADV_LEN 22

/* Ma thiet bi: 4 byte thap cua dia chi identity - co dinh, khac nhau giua cac thiet bi */
static inline uint32_t nav_tag_of(const bt_addr_le_t *id)
{
	return sys_get_le32(id->a.val);
}

static inline void nav_adv_build(uint8_t out[NAV_ADV_LEN], enum nav_dev_type type, uint32_t tag)
{
	static const uint8_t uuid[16] = {NAV_UUID_SVC_VAL};

	memcpy(out, uuid, sizeof(uuid));
	out[16] = NAV_PROTO_VER;
	out[17] = (uint8_t)type;
	sys_put_le32(tag, &out[18]);
}

/* data / len: noi dung mot muc Service Data 128 trong quang ba. true neu la quang ba Dinh vi */
static inline bool nav_adv_parse(const uint8_t *data, uint8_t len, enum nav_dev_type *type,
				 uint32_t *tag)
{
	static const uint8_t uuid[16] = {NAV_UUID_SVC_VAL};

	if (len < NAV_ADV_LEN || memcmp(data, uuid, sizeof(uuid)) != 0 ||
	    data[16] != NAV_PROTO_VER) {
		return false;
	}
	*type = (enum nav_dev_type)data[17];
	*tag = sys_get_le32(&data[18]);
	return true;
}

/* Identity (PUBLIC_ID / RANDOM_ID sau khi giai RPA) -> PUBLIC / RANDOM de so sanh */
static inline void nav_addr_norm(bt_addr_le_t *dst, const bt_addr_le_t *src)
{
	bt_addr_le_copy(dst, src);
	if (dst->type == BT_ADDR_LE_PUBLIC_ID) {
		dst->type = BT_ADDR_LE_PUBLIC;
	} else if (dst->type == BT_ADDR_LE_RANDOM_ID) {
		dst->type = BT_ADDR_LE_RANDOM;
	}
}

#endif /* ME54_PROTO_NAV_PROTO_H_ */
