/*
 * Helmet - chuc nang NHIET DO: lich do MLX90632, loc, canh bao, Health Thermometer Service.
 */

#ifndef FEATURES_THERMO_THERMO_H_
#define FEATURES_THERMO_THERMO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

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

#endif /* FEATURES_THERMO_THERMO_H_ */
