/*
 * Lop DICH VU: phat am bao doc lap (khong giai ma luong am thanh nao) - dung cho xe, noi I2S chi
 * phuc vu am bao. Mu tron am bao vao luong nhac trong services/audio_out.c.
 *
 * 16 kHz stereo, khoi 10 ms; bat dau phat khi da nap 3 khoi. Yeu cau moi lam dung am dang phat
 * (vd am "dang tim" lap).
 */

#ifndef ME54_SERVICES_PROMPT_PLAYER_H_
#define ME54_SERVICES_PROMPT_PLAYER_H_

#include <stdbool.h>

#include <me54/core/prompts.h>

int prompt_player_init(void); /* cau hinh I2S; loi -> khong co am bao */
void prompt_player_play(enum prompt_id id);
void prompt_player_stop(void);
bool prompt_player_busy(void);

#endif /* ME54_SERVICES_PROMPT_PLAYER_H_ */
