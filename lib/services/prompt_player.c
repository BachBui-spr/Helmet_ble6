/*
 * Lop DICH VU - phat am bao doc lap qua I2S (xem prompt_player.h).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <me54/core/prompts.h>
#include <me54/hal/i2s_out.h>
#include <me54/services/prompt_player.h>

#define PP_RATE   16000
#define PP_BLOCK  (PP_RATE / 100) /* 10 ms */
#define PP_BYTES  (PP_BLOCK * 2 * sizeof(int16_t))
#define PP_AMP    4125 /* -18 dBFS */
#define PP_START  3    /* bat dau phat khi da nap 3 khoi */
#define PP_STACK  1536
#define PP_PRIO   K_PRIO_PREEMPT(2)

BUILD_ASSERT(I2S_OUT_BLOCK_BYTES >= PP_BYTES, "CONFIG_ME54_HAL_I2S_OUT_BLOCK_BYTES < 640");

K_MSGQ_DEFINE(pp_q, sizeof(uint8_t), 4, 1);
static atomic_t pp_gen; /* tang moi lan co yeu cau moi: am dang phat dung lai */
static atomic_t pp_busy;
static bool ready;

/* Mot khoi 10 ms. *more = false khi am bao (khong lap) vua het */
static int play_block(struct prompt_gen *g, bool *started, uint8_t *queued, bool *more)
{
	int16_t *blk;
	int err;

	if (i2s_out_alloc((void **)&blk, K_MSEC(100)) != 0) {
		return -ENOMEM;
	}
	/* Mono vao nua dau khoi, roi nhan doi kenh tai cho tu cuoi ve dau (khong can bo dem rieng) */
	*more = prompt_render(g, blk, PP_BLOCK, PP_AMP);
	for (int i = PP_BLOCK - 1; i >= 0; i--) {
		blk[2 * i + 1] = blk[i];
		blk[2 * i] = blk[i];
	}
	err = i2s_out_write(blk, PP_BYTES);
	if (err) {
		i2s_out_free(blk);
		return err;
	}
	if (!*started && ++(*queued) >= PP_START) {
		err = i2s_out_start();
		*started = (err == 0);
	}
	return 0;
}

static void pp_thread(void *p1, void *p2, void *p3)
{
	static struct prompt_gen g;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint8_t id;
		atomic_val_t gen;
		bool started = false;
		bool abort = false;
		bool more = true;
		uint8_t queued = 0;

		(void)k_msgq_get(&pp_q, &id, K_FOREVER);
		if (!ready || id >= PROMPT_COUNT) {
			continue;
		}
		gen = atomic_get(&pp_gen);
		atomic_set(&pp_busy, 1);
		prompt_start(&g, (enum prompt_id)id, PP_RATE);

		while (more) {
			if (atomic_get(&pp_gen) != gen || play_block(&g, &started, &queued, &more) != 0) {
				abort = true;
				break;
			}
		}

		if (started) {
			(void)(abort ? i2s_out_drop() : i2s_out_drain());
		} else if (queued > 0) {
			(void)i2s_out_drop();
		}
		/* Cho I2S tra het khoi (dung han) truoc am ke tiep: ghi luc dang dung se loi */
		i2s_out_wait_released(50, 10);
		atomic_set(&pp_busy, 0);
	}
}

K_THREAD_DEFINE(prompt_player_tid, PP_STACK, pp_thread, NULL, NULL, NULL, PP_PRIO, K_FP_REGS, 0);

int prompt_player_init(void)
{
	int err;

	prompt_init_tables();
	err = i2s_out_init();
	if (err == 0) {
		err = i2s_out_configure(PP_RATE, PP_BLOCK, 100);
	}
	ready = (err == 0);
	return err;
}

void prompt_player_play(enum prompt_id id)
{
	const uint8_t v = (uint8_t)id;

	atomic_inc(&pp_gen); /* dung am dang phat (vd am "dang tim" lap) */
	k_msgq_purge(&pp_q);
	(void)k_msgq_put(&pp_q, &v, K_NO_WAIT);
}

void prompt_player_stop(void)
{
	atomic_inc(&pp_gen);
	k_msgq_purge(&pp_q);
}

bool prompt_player_busy(void)
{
	return atomic_get(&pp_busy) != 0;
}
