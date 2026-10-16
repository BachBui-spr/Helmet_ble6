/*
 * Helmet - dieu khien am luong va media.
 *
 * - VCP Volume Renderer (Helmet la server): dien thoai doc/dat am luong cua Helmet,
 *   nut Vol+/Vol- tren mu doi am luong va dien thoai duoc thong bao lai. Ca hai cach deu
 *   di qua vcs_state_cb -> audio_out_set_volume (mot duong xu ly duy nhat).
 *   Am luong duoc luu vao settings ("helmet/vol") va khoi phuc o lan bat sau.
 * - MCP client (Helmet dieu khien trinh phat nhac tren dien thoai qua GMCS):
 *   nut Call gui PLAY/PAUSE, nhan dup gui NEXT_TRACK.
 * - Do MCS xong (thanh cong hay khong) thi chuyen sang do GTBS (call_ctrl) - noi tiep, khong
 *   chay song song hai lan do GATT.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/mcc.h>
#include <zephyr/bluetooth/audio/mcs.h>
#include <zephyr/bluetooth/audio/media_proxy.h>
#include <zephyr/bluetooth/audio/vcp.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "helmet.h"

LOG_MODULE_REGISTER(media_ctrl, LOG_LEVEL_INF);

#define VOLUME_STEP 16 /* 16 nac tu 0 den 255 */

static struct bt_conn *mcc_conn;
static bool mcs_ready;
static uint8_t media_state = BT_MCS_MEDIA_STATE_INACTIVE;
static uint8_t cur_volume;
static uint8_t cur_mute = BT_VCP_STATE_UNMUTED;

static void mcs_discover_work_handler(struct k_work *work);
static K_WORK_DEFINE(mcs_discover_work, mcs_discover_work_handler);

/* ---------------- VCP Volume Renderer ---------------- */

static void vcs_state_cb(struct bt_conn *conn, int err, uint8_t volume, uint8_t mute)
{
	if (err) {
		LOG_WRN("VCS state error (%d)", err);
		return;
	}

	cur_volume = volume;
	cur_mute = mute;
	audio_out_set_volume(volume, mute == BT_VCP_STATE_MUTED);
	LOG_INF("Volume %u/255 (%u%%)%s%s", volume, (volume * 100U) / 255U,
		mute == BT_VCP_STATE_MUTED ? ", MUTED" : "", conn ? " (phone)" : "");

	if (helmet_cfg.volume != volume) {
		helmet_cfg.volume = volume;
		helmet_cfg_changed();
	}
}

static void vcs_flags_cb(struct bt_conn *conn, int err, uint8_t flags)
{
	if (!err) {
		LOG_DBG("VCS flags 0x%02x", flags);
	}
}

static struct bt_vcp_vol_rend_cb vcp_cbs = {
	.state = vcs_state_cb,
	.flags = vcs_flags_cb,
};

/* ---------------- MCP client ---------------- */

static void mcc_discover_mcs_cb(struct bt_conn *conn, int err)
{
	if (err) {
		LOG_WRN("Media Control Service not found (%d) - play/pause disabled", err);
	} else {
		mcs_ready = true;
		LOG_INF("Media Control Service found on phone");
		(void)bt_mcc_read_media_state(conn);
	}

	/* Do xong MCS -> do tiep GTBS (dich vu goi dien) */
	call_ctrl_connected(conn);
}

static void mcc_read_media_state_cb(struct bt_conn *conn, int err, uint8_t state)
{
	if (err) {
		return;
	}

	/* Duoc goi ca khi doc lan khi dien thoai thong bao doi trang thai */
	media_state = state;
	LOG_INF("Phone media state: %s",
		state == BT_MCS_MEDIA_STATE_PLAYING   ? "playing"
		: state == BT_MCS_MEDIA_STATE_PAUSED  ? "paused"
		: state == BT_MCS_MEDIA_STATE_SEEKING ? "seeking"
						      : "inactive");
}

static void mcc_send_cmd_cb(struct bt_conn *conn, int err, const struct mpl_cmd *cmd)
{
	if (err) {
		LOG_WRN("Media command 0x%02x failed (%d)", cmd->opcode, err);
	}
}

static struct bt_mcc_cb mcc_cb = {
	.discover_mcs = mcc_discover_mcs_cb,
	.read_media_state = mcc_read_media_state_cb,
	.send_cmd = mcc_send_cmd_cb,
};

static void mcs_discover_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (mcc_conn == NULL) {
		return;
	}

	err = bt_mcc_discover_mcs(mcc_conn, true);
	if (err) {
		LOG_WRN("MCS discovery start failed (%d)", err);
		call_ctrl_connected(mcc_conn);
	}
}

static int send_media_cmd(uint8_t opcode)
{
	struct mpl_cmd cmd = {
		.opcode = opcode,
		.use_param = false,
	};

	if (mcc_conn == NULL || !mcs_ready) {
		LOG_WRN("No media control on phone");
		return -ENOTCONN;
	}

	return bt_mcc_send_cmd(mcc_conn, &cmd);
}

/* ---------------- API ---------------- */

/* Goi TRUOC bt_enable(): chi them VCS vao co so du lieu GATT (xem main.c).
 * Luc nay settings chua nap -> dang ky voi am luong mac dinh, media_ctrl_init() dat lai.
 */
int media_ctrl_register(void)
{
	struct bt_vcp_vol_rend_register_param vcp_param = {
		.step = VOLUME_STEP,
		.mute = BT_VCP_STATE_UNMUTED,
		.volume = helmet_cfg.volume,
		.cb = &vcp_cbs,
	};
	int err;

	err = bt_vcp_vol_rend_register(&vcp_param);
	if (err) {
		LOG_ERR("VCP volume renderer register failed (%d)", err);
	}
	return err;
}

/* Goi sau bt_enable() + settings_load(): helmet_cfg.volume da la gia tri da luu */
int media_ctrl_init(void)
{
	int err;

	cur_volume = helmet_cfg.volume;
	err = bt_vcp_vol_rend_set_vol(helmet_cfg.volume);
	if (err) {
		LOG_WRN("VCP restore volume failed (%d)", err);
	}
	audio_out_set_volume(helmet_cfg.volume, false);

	err = bt_mcc_init(&mcc_cb);
	if (err) {
		LOG_ERR("MCC init failed (%d)", err);
		return err;
	}

	LOG_INF("Volume restored: %u/255", helmet_cfg.volume);
	return 0;
}

/* Goi khi lien ket da ma hoa: tim MCS cua dien thoai */
void media_ctrl_connected(struct bt_conn *conn)
{
	if (mcc_conn == NULL) {
		mcc_conn = bt_conn_ref(conn);
		mcs_ready = false;
		k_work_submit(&mcs_discover_work);
	}
}

void media_ctrl_disconnected(void)
{
	if (mcc_conn != NULL) {
		bt_conn_unref(mcc_conn);
		mcc_conn = NULL;
	}
	mcs_ready = false;
	media_state = BT_MCS_MEDIA_STATE_INACTIVE;
}

int media_ctrl_play(void)
{
	return send_media_cmd(BT_MCS_OPC_PLAY);
}

int media_ctrl_pause(void)
{
	return send_media_cmd(BT_MCS_OPC_PAUSE);
}

int media_ctrl_next_track(void)
{
	return send_media_cmd(BT_MCS_OPC_NEXT_TRACK);
}

int media_ctrl_play_pause(void)
{
	const bool playing = media_state == BT_MCS_MEDIA_STATE_PLAYING ||
			     (media_state == BT_MCS_MEDIA_STATE_INACTIVE && le_audio_is_streaming());

	return playing ? media_ctrl_pause() : media_ctrl_play();
}

int media_ctrl_volume_up(void)
{
	if (cur_volume == UINT8_MAX && cur_mute == BT_VCP_STATE_UNMUTED) {
		audio_out_prompt(PROMPT_VOL_LIMIT);
	}
	return bt_vcp_vol_rend_unmute_vol_up();
}

int media_ctrl_volume_down(void)
{
	if (cur_volume == 0) {
		audio_out_prompt(PROMPT_VOL_LIMIT);
	}
	return bt_vcp_vol_rend_vol_down();
}

int media_ctrl_set_volume(uint8_t volume)
{
	return bt_vcp_vol_rend_set_vol(volume);
}

uint8_t media_ctrl_volume(void)
{
	return cur_volume;
}

void media_ctrl_print_status(const struct shell *sh)
{
	shell_print(sh, "Volume    : %u/255 (%u%%)%s", cur_volume, (cur_volume * 100U) / 255U,
		    cur_mute == BT_VCP_STATE_MUTED ? " MUTED" : "");
	shell_print(sh, "Media ctl : %s, phone state %s", mcs_ready ? "ready" : "not available",
		    media_state == BT_MCS_MEDIA_STATE_PLAYING  ? "playing"
		    : media_state == BT_MCS_MEDIA_STATE_PAUSED ? "paused"
							       : "inactive");
}
