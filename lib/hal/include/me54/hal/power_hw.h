/*
 * Lop HAL: nguon cua board (mu va xe dung chung).
 *
 * LDO AP2112K co EN noi thang VIN -> mach luon co 3,3 V khi co pin. "Tat may" la System OFF cua
 * nRF54L15 (sys_poweroff), thuc day bang muc "nhan" cua nut Power -> chip reset va chay lai tu dau
 * main(). VDD do bang SAADC (kenh VDD noi), chi khi bat CONFIG_ADC.
 */

#ifndef ME54_HAL_POWER_HW_H_
#define ME54_HAL_POWER_HW_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

/* Goi dau tien trong main(). Thuc tu System OFF -> nut Power phai con giu du 2 s (tinh tu luc
 * reset), nha som thi tat lai ngay (KHONG tra ve). *cause: nguyen nhan reset; *woke_by_button:
 * vua thuc bang nut. Tra ve true neu nut Power dang duoc giu (hoac vua thuc bang nut).
 */
bool power_hw_boot_check(uint32_t *cause, bool *woke_by_button);

/* System OFF. wait_release: cho nha nut Power truoc (toi da 10 s), vi chip thuc theo MUC nhan.
 * Day het log ra RTT / UART truoc khi tat (RAM se mat).
 */
FUNC_NORETURN void power_hw_system_off(bool wait_release);

/* Do VDD: 0 khi san sang, -ENODEV khi khong co ADC, so am khac khi cau hinh kenh loi */
int power_hw_vdd_init(void);
int power_hw_vdd_mv(void); /* mV, hoac -EIO */

#endif /* ME54_HAL_POWER_HW_H_ */
