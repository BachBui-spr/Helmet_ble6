/*
 * Helmet (apps/main) - khai bao dung chung giua cac module.
 * Thiet ke: docs/HELMET_THIET_KE_CHUC_NANG_MOI.md
 *
 *   main.c       : bo dieu khien ung dung (hang doi su kien, trang thai thiet bi), BLE, shell
 *   le_audio.c   : BAP Unicast Server: 2 ASE sink (nghe) + 1 ASE source (micro)
 *   audio_out.c  : giai ma LC3 -> bu troi clock -> am bao -> lay mau lai co dinh -> I2S
 *   prompts.c    : bo tao am bao (song sin)
 *   media_ctrl.c : VCP Volume Renderer + MCP client (play/pause/next)
 *   call_ctrl.c  : TBS client (nghe/tu choi/ket thuc cuoc goi) + MICP Microphone Device
 *   mic_in.c     : thu 2 micro PDM
 *   voice_dsp.c  : loc thong cao, khu vong NLMS, NLP, giam on, AGC (C thuan)
 *   voice_tx.c   : thread giong noi: micro -> DSP -> bu troi -> LC3 -> gui SDU
 *   buttons.c    : nhan dang cu chi nut
 *   power.c      : bat/tat (System OFF), tu tat, quang ba cham, do VDD
 *   mlx90632.c   : driver MLX90632 + cong thuc Melexis
 *   thermo.c     : lich do nhiet, loc, canh bao, Health Thermometer Service
 *   capture.c    : chep SDU LC3 nhan duoc ra RTT kenh 1
 */

#ifndef HELMET_H_
#define HELMET_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/shell/shell.h>

/* ---------------- Ngu canh, vi tri, gioi han ---------------- */

/* Chieu nhan: nghe nhac + cuoc goi (Conversational) + chuong (Ringtone) */
#define HELMET_SINK_CONTEXTS                                                                       \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL |                \
	 BT_AUDIO_CONTEXT_TYPE_MEDIA | BT_AUDIO_CONTEXT_TYPE_GAME |                                \
	 BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL | BT_AUDIO_CONTEXT_TYPE_SOUND_EFFECTS |               \
	 BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS | BT_AUDIO_CONTEXT_TYPE_RINGTONE |                    \
	 BT_AUDIO_CONTEXT_TYPE_ALERTS)

/* Chieu gui (micro): chi cuoc goi */
#define HELMET_SOURCE_CONTEXTS                                                                     \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)

#define HELMET_SINK_LOCATION   (BT_AUDIO_LOCATION_FRONT_LEFT | BT_AUDIO_LOCATION_FRONT_RIGHT)
#define HELMET_SOURCE_LOCATION BT_AUDIO_LOCATION_FRONT_LEFT

#define HELMET_MAX_STREAMS CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT

/* SDU lon nhat chieu nhan: 2 kenh x 155 octet/khung x 1 khung/SDU */
#define HELMET_MAX_SDU_LEN 320

/* Loi xu ly giong noi luon chay 16 kHz, khung 10 ms */
#define VOICE_RATE  16000
#define VOICE_FRAME 160

/* Cau hinh codec cua mot luong (ASE) da duoc dien thoai chon */
struct helmet_stream_cfg {
	uint32_t freq_hz;
	uint32_t frame_dur_us;
	uint16_t octets_per_frame;
	uint8_t chan_cnt;
	uint32_t chan_alloc; /* enum bt_audio_location, 0 = mono */
};

/* ---------------- Cai dat luu qua lan tat (settings "helmet/...") ---------------- */

struct helmet_cfg {
	uint8_t volume;              /* 0..255, VCP */
	uint16_t autooff_min;        /* tu tat khi khong ket noi, 0 = khong bao gio */
	uint16_t temp_emis_permille; /* do phat xa x 1000 */
	int32_t temp_alarm_mc;       /* nguong canh bao nhiet, milli-do C */
};

extern struct helmet_cfg helmet_cfg;
void helmet_cfg_changed(void); /* hen ghi sau 10 s */
void helmet_cfg_save(void);    /* ghi ngay cac gia tri da doi */

/* ---------------- Bo dieu khien ung dung (main.c) ---------------- */

enum helmet_state {
	HS_BOOT,
	HS_IDLE,      /* bat, dang quang ba, chua ket noi */
	HS_CONNECTED, /* da ket noi (nghe nhac hoac ranh) */
	HS_RINGING,   /* co cuoc goi den */
	HS_IN_CALL,   /* dang goi (hoac dang quay so / giu may) */
	HS_SHUTDOWN,  /* dang tat may */
};

enum helmet_evt_type {
	EVT_KEY_RAW,       /* a = phim, b = 1 nhan / 0 nha, v = thoi diem (ms) */
	EVT_KEY_TIMER,     /* a = phim (KEY_COUNT = hen gio to hop), v = the he hen gio */
	EVT_GESTURE,       /* a = phim, b = cu chi (lenh shell gia lap) */
	EVT_CONNECTED,     /* */
	EVT_DISCONNECTED,  /* a = ly do */
	EVT_SECURED,       /* lien ket da ma hoa */
	EVT_STREAM,        /* a = enum bt_audio_dir, b = 1 bat dau / 0 dung */
	EVT_CALL,          /* a = enum helmet_call (trang thai tong hop) */
	EVT_MIC_MUTE,      /* a = 1 tat / 0 bat */
	EVT_RING_CHECK,    /* 1 s sau khi co cuoc goi den: can chuong cuc bo khong */
	EVT_IDLE_TIMEOUT,  /* het gio tu tat */
	EVT_ADV_SLOWDOWN,  /* het 30 s quang ba nhanh */
	EVT_TEMP_ALARM,    /* a = 1 vuot nguong / 0 tro lai */
	EVT_LOW_BATTERY,   /* a = 1 canh bao / 2 phai tat */
	EVT_POWER_OFF_REQ, /* lenh shell */
};

/* ---------------- status_led.c ---------------- */
int status_led_init(void);
void status_led_set_state(enum helmet_state state);
void status_led_shutdown_wait(void);

struct helmet_evt {
	uint8_t type;
	uint8_t a;
	uint8_t b;
	uint8_t pad;
	uint32_t v;
};

/* Goi duoc tu moi thread / ISR, khong bao gio cho */
void app_post(uint8_t type, uint8_t a, uint8_t b, uint32_t v);
enum helmet_state app_state(void);
const char *app_state_str(enum helmet_state s);
/* buttons.c -> main.c, chay trong thread bo dieu khien */
void app_gesture(uint8_t key, uint8_t gesture);
/* power.c: ngat ket noi (cho toi 1 s) + dung quang ba */
void app_bt_shutdown(void);
void app_adv_set_slow(bool slow);

/* ---------------- buttons.c ---------------- */

enum helmet_key { KEY_VOL_UP, KEY_VOL_DOWN, KEY_CALL, KEY_POWER, KEY_COUNT };
enum helmet_gesture { G_SHORT, G_DOUBLE, G_LONG, G_VERY_LONG, G_REPEAT, G_COMBO_VOL, G_COUNT };

void buttons_init(void);
void buttons_handle(const struct helmet_evt *evt); /* chi goi tu bo dieu khien */
void buttons_set_double(enum helmet_key key, bool enabled);
void buttons_ignore_until_release(enum helmet_key key);
const char *buttons_key_str(uint8_t key);
const char *buttons_gesture_str(uint8_t gesture);
int buttons_key_from_str(const char *s);
int buttons_gesture_from_str(const char *s);

/* ---------------- audio_out.c + prompts.c ---------------- */

struct helmet_audio_stats {
	uint32_t sdu_rx;       /* SDU nhan duoc */
	uint32_t sdu_bad;      /* SDU loi/mat (da bu bang PLC) */
	uint32_t plc;          /* so kenh-khung phai noi suy */
	uint32_t dec_err;      /* loi giai ma */
	uint32_t len_err;      /* SDU sai do dai */
	uint32_t q_full;       /* SDU bi bo vi hang doi day */
	uint32_t slip_drop;    /* mau bi bo de bu troi clock dien thoai */
	uint32_t slip_insert;  /* mau duoc chen them */
	uint32_t underrun;     /* khung phai phat im lang vi het du lieu */
	uint32_t ring_drop;    /* khung bi bo vi vong PCM day */
	uint32_t i2s_err;      /* loi I2S (da tu khoi phuc) */
	uint32_t out_rate;     /* tan so danh dinh dang phat */
	uint32_t i2s_rate_mhz; /* tan so I2S that (mHz) */
	uint16_t frame_samples;
	uint16_t ring_level;   /* mau stereo dang cho trong vong PCM */
	uint8_t active_mask;   /* BIT(0) = kenh trai, BIT(1) = kenh phai dang nhan */
	uint8_t queued_blocks; /* so khoi PCM dang cho trong I2S */
	bool i2s_running;
	bool prompt_active;
};

enum helmet_prompt {
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
	PROMPT_COUNT,
};

int audio_out_init(void);
void audio_out_stream_start(uint8_t idx, const struct helmet_stream_cfg *cfg);
void audio_out_stream_stop(uint8_t idx);
void audio_out_stream_data(uint8_t idx, bool valid, uint32_t ts, const uint8_t *data,
			   uint16_t len);
void audio_out_set_volume(uint8_t volume, bool mute);
void audio_out_prompt(enum helmet_prompt p);
void audio_out_prompt_stop(void);
bool audio_out_prompt_busy(void);
void audio_out_shutdown(void);
void audio_out_get_stats(struct helmet_audio_stats *stats);
void audio_out_print_perf(const struct shell *sh);

/* Tham chieu cho bo khu vong, mien 16 kHz.
 * anchor: mau tham chieu thu ref_pos bat dau phat ra loa tai thoi diem t_cyc (k_cycle_get_32).
 * gen doi moi khi dau ra khoi dong lai -> ben doc phai tinh lai do lech.
 */
bool audio_out_ref_anchor(uint32_t *ref_pos, uint32_t *t_cyc, uint32_t *gen);
/* Tra ve so mau that co (cac mau thieu duoc ghi 0) */
int audio_out_ref_read(int16_t *dst, uint16_t n, uint32_t pos);

/* Bo tao am bao (prompts.c), dung trong thread audio */
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
void prompt_start(struct prompt_gen *g, enum helmet_prompt p, uint32_t rate);
void prompt_set_rate(struct prompt_gen *g, uint32_t rate);
void prompt_stop(struct prompt_gen *g);
/* Ghi n mau mono vao dst; tra ve true neu am bao con dang phat */
bool prompt_render(struct prompt_gen *g, int16_t *dst, uint16_t n, uint16_t amp);
const char *prompt_name(int p);
int prompt_from_name(const char *name);

/* ---------------- le_audio.c ---------------- */

struct helmet_rx_flags {
	uint32_t ok;    /* du lieu tot */
	uint32_t crc;   /* BT_ISO_FLAGS_ERROR: nhan duoc nhung co the hong */
	uint32_t lost;  /* BT_ISO_FLAGS_LOST: khong nhan duoc gi */
	uint32_t empty; /* hop le nhung do dai 0 */
};

int le_audio_register(void); /* truoc bt_enable(): PACS + ASCS vao GATT */
int le_audio_init(void);
bool le_audio_is_streaming(void);        /* co it nhat 1 ASE sink dang phat */
bool le_audio_source_streaming(void);    /* ASE source (micro) dang phat */
void le_audio_set_source_available(bool available);
void le_audio_print_status(const struct shell *sh);
const char *le_audio_phy_str(uint8_t phy);
void le_audio_set_qos_pref(uint8_t phy, uint8_t rtn, uint16_t latency_ms);
void le_audio_get_rx_flags(struct helmet_rx_flags *out);
void le_audio_print_link_quality(const struct shell *sh);

/* ---------------- media_ctrl.c ---------------- */

int media_ctrl_register(void); /* truoc bt_enable(): VCS vao GATT */
int media_ctrl_init(void);
void media_ctrl_connected(struct bt_conn *conn);
void media_ctrl_disconnected(void);
int media_ctrl_play_pause(void);
int media_ctrl_play(void);
int media_ctrl_pause(void);
int media_ctrl_next_track(void);
int media_ctrl_volume_up(void);
int media_ctrl_volume_down(void);
int media_ctrl_set_volume(uint8_t volume);
uint8_t media_ctrl_volume(void);
void media_ctrl_print_status(const struct shell *sh);

/* ---------------- call_ctrl.c ---------------- */

enum helmet_call { CALL_NONE, CALL_INCOMING, CALL_OUTGOING, CALL_ACTIVE, CALL_HELD };

int call_ctrl_register(void); /* truoc bt_enable(): MICS vao GATT */
int call_ctrl_init(void);
void call_ctrl_connected(struct bt_conn *conn); /* bat dau do GTBS */
void call_ctrl_disconnected(void);
int call_ctrl_accept(void);
int call_ctrl_reject(void);
int call_ctrl_hangup(void);
int call_ctrl_mic_mute(bool mute);
void call_ctrl_mic_unavailable(bool unavailable);
enum helmet_call call_ctrl_state(void);
const char *call_ctrl_state_str(enum helmet_call c);
void call_ctrl_print_status(const struct shell *sh);

/* ---------------- mic_in.c ---------------- */

int mic_in_init(void);
bool mic_in_ready(void);
int mic_in_start(uint32_t *t0_cyc);
/* Dung + chay lai (driver tu dung PDM khi hang doi day). t0_cyc: thoi diem bat dau moi */
int mic_in_restart(uint32_t *t0_cyc);
/* Doc 1 khoi 10 ms: 160 mau x 2 kenh (L, R xen ke) */
int mic_in_read(int16_t **block, int32_t timeout_ms);
void mic_in_free(int16_t *block);
void mic_in_stop(void);

/* ---------------- voice_tx.c ---------------- */

struct helmet_voice_stats {
	uint32_t sessions;
	uint32_t mic_blocks;
	uint32_t mic_err;
	uint32_t mic_restart;  /* micro duoc khoi dong lai sau loi doc lien tiep */
	uint32_t sdu_sent;
	uint32_t sdu_underrun; /* het SDU khi controller can -> gui im lang */
	uint32_t send_err;
	uint32_t pool_empty;
	uint32_t q_drop;       /* SDU bi bo vi hang day */
	uint32_t slip_drop;    /* mau bo de bu troi clock */
	uint32_t slip_insert;  /* mau chen */
	uint32_t ref_missing;  /* khoi khong co tham chieu */
	int16_t mic_dbfs_x10;
	int16_t out_dbfs_x10;
	int16_t erle_db_x10;
	uint32_t out_rate;
	bool active;
	bool muted;
	bool mic_ok;
};

void voice_start(struct bt_bap_stream *stream, const struct helmet_stream_cfg *cfg);
void voice_stop(void);
void voice_on_sent(struct bt_bap_stream *stream);
void voice_set_mute(bool mute);
bool voice_is_muted(void);
void voice_set_dsp(bool aec, bool ns, bool agc);
void voice_get_dsp(bool *aec, bool *ns, bool *agc);
void voice_get_stats(struct helmet_voice_stats *st);
void voice_print_status(const struct shell *sh);
void voice_print_perf(const struct shell *sh);

/* ---------------- power.c ---------------- */

enum power_off_reason { OFF_BUTTON, OFF_IDLE, OFF_LOW_BATTERY, OFF_SHELL };

/* Goi dau tien trong main(). Tra ve true neu may duoc bat bang nut Power (nut co the
 * van dang giu). Co the KHONG tra ve: thuc day nham -> tat lai ngay.
 */
bool power_boot_check(void);
FUNC_NORETURN void power_off(enum power_off_reason why);
void power_state_changed(enum helmet_state s);
void power_activity(void);
void power_autooff_changed(void);
int power_vdd_mv(void);
void power_print_status(const struct shell *sh);

/* ---------------- thermo.c ---------------- */

struct helmet_temp {
	int32_t object_mc;  /* nhiet do vat the, milli-do C */
	int32_t ambient_mc; /* nhiet do moi truong (cua cam bien) */
	uint32_t age_ms;
	bool valid;
	bool alarm;
};

int thermo_init(void);
void thermo_get(struct helmet_temp *out);
void thermo_sleep(void);
int thermo_shell(const struct shell *sh, size_t argc, char **argv);

/* ---------------- capture.c (gui SDU LC3 nhan duoc ve PC qua RTT kenh 1) ---------------- */

int capture_init(void);
void capture_set_enabled(bool on);
void capture_stream_start(uint8_t idx, const struct helmet_stream_cfg *cfg);
void capture_stream_stop(uint8_t idx);
void capture_sdu(uint8_t idx, uint8_t iso_flags, uint16_t iso_seq, uint32_t ts,
		 const uint8_t *data, uint16_t len);
void capture_print_status(const struct shell *sh);

#endif /* HELMET_H_ */
