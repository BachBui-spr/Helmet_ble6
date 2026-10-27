/*
 * Helmet - CHAN DOAN tam thoi: log moi lan thiet bi kia doc / ghi thuoc tinh GATT.
 */

#ifndef DEBUG_GATT_TRACE_H_
#define DEBUG_GATT_TRACE_H_

#include <stddef.h>

#include <zephyr/shell/shell.h>

int gatt_trace_init(void); /* sau bt_enable() */
int gatt_trace_shell(const struct shell *sh, size_t argc, char **argv);

#endif /* DEBUG_GATT_TRACE_H_ */
