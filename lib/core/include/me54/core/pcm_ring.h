/*
 * Lop CORE: vong PCM stereo + bu troi clock nguon.
 *
 * Nguon (dien thoai, broadcast) day khung theo clock cua no; ben doc lay theo nhip dau ra. Muc
 * day TRUNG BINH lech khoi muc tieu (chot sau ~1 s on dinh) thi lay n + 1 hoac n - 1 mau cho
 * mot khung n mau (noi suy tuyen tinh) - tai khong phan biet duoc.
 */

#ifndef ME54_CORE_PCM_RING_H_
#define ME54_CORE_PCM_RING_H_

#include <stdbool.h>
#include <stdint.h>

struct pcm_ring {
	int16_t *buf;  /* cap mau stereo (L, R xen ke) */
	uint32_t cap;
	uint32_t rd;
	uint32_t wr;
	int32_t level_avg_x32;
	int32_t level_target;
	uint16_t settle;
	bool on; /* da du du lieu, dang lay tu vong */
};

void pcm_ring_init(struct pcm_ring *r, int16_t *buf, uint32_t cap);
void pcm_ring_reset(struct pcm_ring *r);

static inline uint32_t pcm_ring_count(const struct pcm_ring *r)
{
	return r->wr - r->rd;
}

/* Them n mau stereo; vong day thi bo n mau cu nhat va tra ve true. Lan dau co du start_level
 * mau thi bat dau lay (on = true).
 */
bool pcm_ring_push(struct pcm_ring *r, const int16_t *src, uint16_t n, uint32_t start_level);
/* So mau vao can lay cho n mau ra: n - 1 (chen 1), n, n + 1 (bo 1). Goi 1 lan moi khung */
uint16_t pcm_ring_slip_take(struct pcm_ring *r, uint16_t n);
/* Lay 'take' mau, co / gian thanh n mau (noi suy tuyen tinh) */
void pcm_ring_pull(struct pcm_ring *r, int16_t *dst, uint16_t n, uint16_t take);

#endif /* ME54_CORE_PCM_RING_H_ */
