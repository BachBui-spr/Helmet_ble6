/*
 * Lop HAL: LED trang thai (led0) voi cac kieu nhay. Ung dung chon kieu theo trang thai cua no.
 */

#ifndef ME54_HAL_STATUS_LED_H_
#define ME54_HAL_STATUS_LED_H_

enum status_led_pattern {
	LED_PAT_BOOT,     /* 3 lan nhay nhanh roi tat */
	LED_PAT_IDLE,     /* nhay ngan moi 2 s */
	LED_PAT_ON,       /* sang lien tuc */
	LED_PAT_RING,     /* nhay nhanh 2 Hz */
	LED_PAT_CALL,     /* nhay ngan moi 1 s */
	LED_PAT_SHUTDOWN, /* sang 1 s roi tat */
};

int status_led_init(void);
void status_led_set(enum status_led_pattern p);
/* Cho kieu LED_PAT_SHUTDOWN chay xong (toi da ~1,1 s) */
void status_led_shutdown_wait(void);

#endif /* ME54_HAL_STATUS_LED_H_ */
