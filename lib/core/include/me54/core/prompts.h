/*
 * Lop CORE: am bao (song sin) dung chung cho mu va xe.
 *
 * Moi am bao la danh sach doan {tan so, thoi luong, khoang lang}. Song sin tao bang bo tich luy
 * pha 32 bit + bang 256 mau, noi suy tuyen tinh; moi doan co doc len / xuong 5 ms. Am bao co the
 * lap lai (chuong, "dang tim") cho toi khi bi dung. Moi thoi luong la boi so cua 10 ms.
 */

#ifndef ME54_CORE_PROMPTS_H_
#define ME54_CORE_PROMPTS_H_

#include <stdbool.h>
#include <stdint.h>

enum prompt_id {
	PROMPT_POWER_ON,
	PROMPT_POWER_OFF,
	PROMPT_CONNECTED,
	PROMPT_DISCONNECTED,
	PROMPT_RING,
	PROMPT_MIC_MUTED,
	PROMPT_MIC_UNMUTED,
	PROMPT_VOL_LIMIT,
	PROMPT_PAIRING,
	PROMPT_TEMP_ALARM,
	PROMPT_LOW_BATTERY,
	PROMPT_STATUS_OK,
	PROMPT_STATUS_NO_LINK,
	PROMPT_IC_SEARCH,   /* lap: master dang tim (2 tieng di len) */
	PROMPT_IC_WAIT,     /* lap: slave dang cho (2 tieng di xuong) */
	PROMPT_IC_LINKED,   /* da ket noi voi mu ban */
	PROMPT_IC_END,      /* ket thuc / huy intercom */
	PROMPT_IC_LOST,     /* mat lien ket, khong noi lai duoc */
	PROMPT_IC_UNAVAIL,  /* het 60 s khong tim thay / khong duoc ket noi */
	PROMPT_BC_SEARCH,   /* lap: dang tu tim broadcast Auracast (3 tieng di len) */
	PROMPT_BC_JOINED,   /* da nhan duoc am thanh broadcast */
	PROMPT_BC_LEFT,     /* roi broadcast */
	PROMPT_BC_FAIL,     /* khong tim thay / khong vao duoc broadcast */
	PROMPT_COUNT,
};

/* Trang thai bo tao am bao (nguoi goi giu, vd thread audio) */
struct prompt_gen {
	const void *def;
	uint32_t rate;
	uint32_t pos;    /* mau trong doan hien tai */
	uint32_t tone;   /* so mau co tieng cua doan */
	uint32_t total;  /* tone + khoang lang */
	uint32_t ramp;
	uint32_t phase;
	uint32_t inc;
	uint8_t seg;
	uint8_t id;
	bool active;
	bool loop_gap;
};

void prompt_init_tables(void);
void prompt_start(struct prompt_gen *g, enum prompt_id p, uint32_t rate);
void prompt_set_rate(struct prompt_gen *g, uint32_t rate);
void prompt_stop(struct prompt_gen *g);
/* Ghi n mau mono vao dst; tra ve true neu am bao con dang phat */
bool prompt_render(struct prompt_gen *g, int16_t *dst, uint16_t n, uint16_t amp);
const char *prompt_name(int p);
int prompt_from_name(const char *name);

#endif /* ME54_CORE_PROMPTS_H_ */
