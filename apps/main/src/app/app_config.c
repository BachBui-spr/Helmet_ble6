/*
 * Helmet (apps/main) - lop UNG DUNG: cai dat luu qua lan tat (settings "helmet/...").
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "app/app_internal.h"

/* ---------------- Cai dat luu qua lan tat ---------------- */

struct helmet_cfg helmet_cfg = {
	.volume = 128,
	.autooff_min = 10,
	.temp_emis_permille = 980,
	.temp_alarm_mc = 38000,
};
static struct helmet_cfg saved_cfg;

static int cfg_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	void *dst = NULL;
	size_t size = 0;

	if (settings_name_steq(key, "vol", &next) && !next) {
		dst = &helmet_cfg.volume;
		size = sizeof(helmet_cfg.volume);
	} else if (settings_name_steq(key, "autooff", &next) && !next) {
		dst = &helmet_cfg.autooff_min;
		size = sizeof(helmet_cfg.autooff_min);
	} else if (settings_name_steq(key, "emis", &next) && !next) {
		dst = &helmet_cfg.temp_emis_permille;
		size = sizeof(helmet_cfg.temp_emis_permille);
	} else if (settings_name_steq(key, "alarm", &next) && !next) {
		dst = &helmet_cfg.temp_alarm_mc;
		size = sizeof(helmet_cfg.temp_alarm_mc);
	} else {
		return -ENOENT;
	}

	if (len != size || read_cb(cb_arg, dst, size) != (ssize_t)size) {
		return -EINVAL;
	}
	return 0;
}

static int cfg_commit(void)
{
	saved_cfg = helmet_cfg;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(helmet, "helmet", NULL, cfg_set, cfg_commit, NULL);

void helmet_cfg_save(void)
{
	if (helmet_cfg.volume != saved_cfg.volume) {
		(void)settings_save_one("helmet/vol", &helmet_cfg.volume, sizeof(helmet_cfg.volume));
	}
	if (helmet_cfg.autooff_min != saved_cfg.autooff_min) {
		(void)settings_save_one("helmet/autooff", &helmet_cfg.autooff_min,
					sizeof(helmet_cfg.autooff_min));
	}
	if (helmet_cfg.temp_emis_permille != saved_cfg.temp_emis_permille) {
		(void)settings_save_one("helmet/emis", &helmet_cfg.temp_emis_permille,
					sizeof(helmet_cfg.temp_emis_permille));
	}
	if (helmet_cfg.temp_alarm_mc != saved_cfg.temp_alarm_mc) {
		(void)settings_save_one("helmet/alarm", &helmet_cfg.temp_alarm_mc,
					sizeof(helmet_cfg.temp_alarm_mc));
	}
	saved_cfg = helmet_cfg;
}

static void cfg_save_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	helmet_cfg_save();
}
static K_WORK_DELAYABLE_DEFINE(cfg_save_work, cfg_save_work_fn);

void helmet_cfg_changed(void)
{
	/* Gom nhieu thay doi (vd giu Vol+) thanh 1 lan ghi RRAM */
	k_work_reschedule(&cfg_save_work, K_SECONDS(10));
}

void helmet_cfg_loaded(void)
{
	saved_cfg = helmet_cfg;
}
