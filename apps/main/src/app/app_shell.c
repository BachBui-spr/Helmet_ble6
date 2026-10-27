/*
 * Helmet (apps/main) - lop UNG DUNG: lenh shell "helmet ..." (RTT / UART20).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <me54/core/prompts.h>
#include <me54/services/audio_out.h>
#include <me54/services/buttons.h>
#include <me54/services/voice_tx.h>

#include "app/app_internal.h"
#include "features/broadcast/broadcast.h"
#include "features/intercom/intercom.h"
#include "features/nav/nav.h"
#include "features/phone/call_ctrl.h"
#include "features/phone/le_audio.h"
#include "features/phone/media_ctrl.h"
#include "features/thermo/thermo.h"
#include "debug/capture.h"
#include "debug/gatt_trace.h"

/* ---------------- Shell ---------------- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char addr[BT_ADDR_LE_STR_LEN] = "-";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (app_bt_phone() != NULL) {
		bt_addr_le_to_str(bt_conn_get_dst(app_bt_phone()), addr, sizeof(addr));
	}
	shell_print(sh, "State     : %s", app_state_str(app_state()));
	shell_print(sh, "Connection: %s%s (security %d)", addr,
		    app_bt_phone() ? "" : (app_bt_adv_slow() ? " (advertising slow)" : " (advertising)"),
		    app_bt_phone() ? app_bt_sec_level() : 0);
	media_ctrl_print_status(sh);
	call_ctrl_print_status(sh);
	le_audio_print_status(sh);
	bcast_print(sh);
	voice_print_status(sh);
	power_print_status(sh);
	return 0;
}

static int cmd_vol(const struct shell *sh, size_t argc, char **argv)
{
	const long v = strtol(argv[1], NULL, 10);

	if (v < 0 || v > 255) {
		shell_error(sh, "usage: helmet vol <0..255>");
		return -EINVAL;
	}
	return media_ctrl_set_volume((uint8_t)v);
}

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_play();
}

static int cmd_pause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_pause();
}

static int cmd_next(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return media_ctrl_next_track();
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (err) {
		shell_error(sh, "unpair failed (%d)", err);
		return err;
	}
	shell_print(sh, "All bonds deleted - xoa ca \"%s\" tren dien thoai truoc khi ghep lai",
		    CONFIG_BT_DEVICE_NAME);
	return 0;
}

static int cmd_capture(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			capture_set_enabled(true);
		} else if (strcmp(argv[1], "off") == 0) {
			capture_set_enabled(false);
		} else {
			shell_error(sh, "usage: helmet capture [on|off]");
			return -EINVAL;
		}
	}
	capture_print_status(sh);
	return 0;
}

static int parse_phy(const char *s, uint8_t *phy)
{
	if (strcmp(s, "1m") == 0) {
		*phy = BT_GAP_LE_PHY_1M;
	} else if (strcmp(s, "2m") == 0) {
		*phy = BT_GAP_LE_PHY_2M;
	} else if (strcmp(s, "coded") == 0) {
		*phy = BT_GAP_LE_PHY_CODED;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int cmd_cispref(const struct shell *sh, size_t argc, char **argv)
{
	const long rtn = strtol(argv[2], NULL, 10);
	const long latency = strtol(argv[3], NULL, 10);
	uint8_t phy;

	if (parse_phy(argv[1], &phy) != 0 || phy == BT_GAP_LE_PHY_CODED || rtn < 0 || rtn > 15 ||
	    latency < 5 || latency > 4000) {
		shell_error(sh, "usage: helmet cispref <1m|2m> <rtn 0..15> <latency_ms 5..4000>");
		return -EINVAL;
	}
	le_audio_set_qos_pref(phy, (uint8_t)rtn, (uint16_t)latency);
	return 0;
}

static int cmd_btn(const struct shell *sh, size_t argc, char **argv)
{
	const int key = buttons_key_from_str(argv[1]);
	const int g = buttons_gesture_from_str(argv[2]);

	if (key < 0 || g < 0) {
		shell_error(sh, "usage: helmet btn <volup|voldown|call|power> "
				"<short|double|long|vlong|repeat|combo>");
		return -EINVAL;
	}
	app_post(EVT_GESTURE, (uint8_t)key, (uint8_t)g, 0);
	return 0;
}

static int cmd_call(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;

	if (argc > 1) {
		if (strcmp(argv[1], "accept") == 0) {
			err = call_ctrl_accept();
		} else if (strcmp(argv[1], "reject") == 0) {
			err = call_ctrl_reject();
		} else if (strcmp(argv[1], "end") == 0) {
			err = call_ctrl_hangup();
		} else {
			shell_error(sh, "usage: helmet call [accept|reject|end]");
			return -EINVAL;
		}
		if (err) {
			shell_error(sh, "failed (%d)", err);
		}
	}
	call_ctrl_print_status(sh);
	return err;
}

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "mute") == 0) {
			return call_ctrl_mic_mute(true);
		} else if (strcmp(argv[1], "unmute") == 0) {
			return call_ctrl_mic_mute(false);
		}
		shell_error(sh, "usage: helmet mic [mute|unmute]");
		return -EINVAL;
	}
	shell_print(sh, "Microphone %s", voice_is_muted() ? "MUTED" : "unmuted");
	return 0;
}

static int cmd_voice(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	voice_print_status(sh);
	return 0;
}

/* helmet dsp <aec|ns|agc> <on|off> */
static int cmd_dsp(const struct shell *sh, size_t argc, char **argv)
{
	bool aec;
	bool ns;
	bool agc;
	bool on;

	voice_get_dsp(&aec, &ns, &agc);
	if (argc < 3) {
		shell_print(sh, "AEC %s, NS %s, AGC %s", aec ? "on" : "off", ns ? "on" : "off",
			    agc ? "on" : "off");
		return 0;
	}
	on = strcmp(argv[2], "on") == 0;
	if (strcmp(argv[1], "aec") == 0) {
		aec = on;
	} else if (strcmp(argv[1], "ns") == 0) {
		ns = on;
	} else if (strcmp(argv[1], "agc") == 0) {
		agc = on;
	} else {
		shell_error(sh, "usage: helmet dsp <aec|ns|agc> <on|off>");
		return -EINVAL;
	}
	voice_set_dsp(aec, ns, agc);
	shell_print(sh, "AEC %s, NS %s, AGC %s", aec ? "on" : "off", ns ? "on" : "off",
		    agc ? "on" : "off");
	return 0;
}

static int cmd_prompt(const struct shell *sh, size_t argc, char **argv)
{
	const int p = prompt_from_name(argv[1]);

	if (strcmp(argv[1], "stop") == 0) {
		audio_out_prompt_stop();
		return 0;
	}
	if (p < 0) {
		shell_error(sh, "unknown prompt; one of:");
		for (int i = 0; i < PROMPT_COUNT; i++) {
			shell_print(sh, "  %s", prompt_name(i));
		}
		return -EINVAL;
	}
	if (p == PROMPT_RING) {
		shell_print(sh, "ring repeats - stop with: helmet prompt stop");
	}
	audio_out_prompt((enum prompt_id)p);
	return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "off") == 0) {
		app_post(EVT_POWER_OFF_REQ, 0, 0, 0);
		return 0;
	}
	power_print_status(sh);
	return 0;
}

static int cmd_autooff(const struct shell *sh, size_t argc, char **argv)
{
	const long v = strtol(argv[1], NULL, 10);

	if (v < 0 || v > 240) {
		shell_error(sh, "usage: helmet autooff <minutes 0..240> (0 = never)");
		return -EINVAL;
	}
	helmet_cfg.autooff_min = (uint16_t)v;
	helmet_cfg_changed();
	power_autooff_changed();
	shell_print(sh, "Auto power off: %ld min", v);
	return 0;
}

static int cmd_ic(const struct shell *sh, size_t argc, char **argv)
{
	return intercom_shell(sh, argc, argv);
}

static int cmd_nav(const struct shell *sh, size_t argc, char **argv)
{
	return nav_shell(sh, argc, argv);
}

static int cmd_trace(const struct shell *sh, size_t argc, char **argv)
{
	return gatt_trace_shell(sh, argc, argv);
}

static int cmd_bc(const struct shell *sh, size_t argc, char **argv)
{
	return bcast_shell(sh, argc, argv);
}

static int cmd_perf(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	audio_out_print_perf(sh);
	voice_print_perf(sh);
	return 0;
}

/* CPU that cua tung thread trong mot cua so thoi gian (CONFIG_THREAD_RUNTIME_STATS).
 * Khac "helmet perf" (do thoi gian tu dau toi cuoi, ke ca luc bi thread khac chen ngang).
 * Thoi gian ngat duoc tinh vao thread dang chay luc ngat xay ra.
 */
#define CPU_MAX_THREADS 24
#define CPU_WINDOW_MS   2000

struct cpu_snap {
	const struct k_thread *t;
	uint64_t cyc;
};

struct cpu_ctx {
	struct cpu_snap s[CPU_MAX_THREADS];
	uint8_t n;
};

static void cpu_collect(const struct k_thread *t, void *user_data)
{
	struct cpu_ctx *c = user_data;
	k_thread_runtime_stats_t rs;

	if (c->n < CPU_MAX_THREADS && k_thread_runtime_stats_get((k_tid_t)t, &rs) == 0) {
		c->s[c->n].t = t;
		c->s[c->n].cyc = rs.execution_cycles;
		c->n++;
	}
}

static int cmd_cpu(const struct shell *sh, size_t argc, char **argv)
{
	static struct cpu_ctx before;
	static struct cpu_ctx after;
	const uint32_t ms = (argc > 1) ? strtoul(argv[1], NULL, 10) : CPU_WINDOW_MS;
	uint64_t window;
	uint64_t busy = 0;

	if (ms < 100 || ms > 60000) {
		shell_error(sh, "Window: 100..60000 ms");
		return -EINVAL;
	}

	before.n = 0;
	k_thread_foreach_unlocked(cpu_collect, &before);
	k_msleep(ms);
	after.n = 0;
	k_thread_foreach_unlocked(cpu_collect, &after);

	window = (uint64_t)ms * sys_clock_hw_cycles_per_sec() / MSEC_PER_SEC;
	shell_print(sh, "CPU over %u ms (128 MHz):", ms);
	for (int i = 0; i < after.n; i++) {
		const char *name = k_thread_name_get((k_tid_t)after.s[i].t);
		uint64_t d = after.s[i].cyc;
		uint32_t pct_x10;

		for (int j = 0; j < before.n; j++) {
			if (before.s[j].t == after.s[i].t) {
				d -= before.s[j].cyc;
				break;
			}
		}
		pct_x10 = (uint32_t)(d * 1000U / window);
		if (name == NULL || name[0] == '\0') {
			name = "?";
		}
		if (strcmp(name, "idle") != 0) {
			busy += d;
		}
		if (pct_x10 > 0) {
			shell_print(sh, "  %-20s %3u.%u %%", name, pct_x10 / 10U, pct_x10 % 10U);
		}
	}
	busy = busy * 1000U / window;
	shell_print(sh, "  %-20s %3u.%u %%", "TOTAL (non-idle)", (uint32_t)busy / 10U,
		    (uint32_t)busy % 10U);
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	return thermo_shell(sh, argc, argv);
}

static int cmd_vdd(const struct shell *sh, size_t argc, char **argv)
{
	const int mv = power_vdd_mv();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (mv < 0) {
		shell_error(sh, "VDD not available (%d)", mv);
		return mv;
	}
	shell_print(sh, "VDD %d mV", mv);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(helmet_cmds,
	SHELL_CMD(status, NULL, "State, connection, volume, calls, streams, voice, power",
		  cmd_status),
	SHELL_CMD_ARG(vol, NULL, "Set volume: helmet vol <0..255>", cmd_vol, 2, 0),
	SHELL_CMD(play, NULL, "Send PLAY to phone", cmd_play),
	SHELL_CMD(pause, NULL, "Send PAUSE to phone", cmd_pause),
	SHELL_CMD(next, NULL, "Send NEXT TRACK to phone", cmd_next),
	SHELL_CMD(unpair, NULL, "Delete all bonds", cmd_unpair),
	SHELL_CMD_ARG(capture, NULL,
		      "Send received LC3 audio to PC over RTT channel 1: helmet capture [on|off]",
		      cmd_capture, 1, 1),
	SHELL_CMD_ARG(cispref, NULL,
		      "Test: preferred CIS QoS: helmet cispref <1m|2m> <rtn> <latency_ms>",
		      cmd_cispref, 4, 0),
	SHELL_CMD_ARG(btn, NULL,
		      "Simulate gesture: helmet btn <volup|voldown|call|power> "
		      "<short|double|long|vlong|repeat|combo>",
		      cmd_btn, 3, 0),
	SHELL_CMD_ARG(call, NULL, "Calls: helmet call [accept|reject|end]", cmd_call, 1, 1),
	SHELL_CMD_ARG(mic, NULL, "Microphone: helmet mic [mute|unmute]", cmd_mic, 1, 1),
	SHELL_CMD(voice, NULL, "Uplink (microphone) statistics", cmd_voice),
	SHELL_CMD_ARG(dsp, NULL, "Voice DSP: helmet dsp [<aec|ns|agc> <on|off>]", cmd_dsp, 1, 2),
	SHELL_CMD_ARG(prompt, NULL, "Play prompt: helmet prompt <name>|stop", cmd_prompt, 2, 0),
	SHELL_CMD_ARG(power, NULL, "Power status, or: helmet power off", cmd_power, 1, 1),
	SHELL_CMD_ARG(autooff, NULL, "Auto power off: helmet autooff <minutes> (0 = never)",
		      cmd_autooff, 2, 0),
	SHELL_CMD(perf, NULL, "CPU time of decode / output / voice DSP / encode", cmd_perf),
	SHELL_CMD_ARG(cpu, NULL, "CPU load per thread: helmet cpu [window_ms] (default 2000)",
		      cmd_cpu, 1, 1),
	SHELL_CMD_ARG(ic, NULL,
		      "Intercom: helmet ic [status|master|slave|stop|unpair|tone <Hz>|off]", cmd_ic,
		      1, 2),
	SHELL_CMD_ARG(bc, NULL,
		      "Auracast: helmet bc [status|scan [name]|stop|forget|code <text>|code clear]", cmd_bc,
		      1, 2),
	SHELL_CMD_ARG(nav, NULL,
		      "Navigation (distances to bike / other helmet): helmet nav [status|on|off|"
		      "ch <1|2|4>|period <ms>]",
		      cmd_nav, 1, 2),
	SHELL_CMD_ARG(trace, NULL, "Diagnostic GATT access log: helmet trace [on|off]", cmd_trace,
		      1, 1),
	SHELL_CMD_ARG(temp, NULL,
		      "Temperature: helmet temp [raw|cal|emis <permille>|alarm <C>]", cmd_temp,
		      1, 2),
	SHELL_CMD(vdd, NULL, "Measure supply voltage (VDD)", cmd_vdd),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(helmet, &helmet_cmds, "Helmet LE Audio headset", NULL);

