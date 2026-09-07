// SPDX-License-Identifier: GPL-2.0+
/*
 * status_led_init.c - light the board status LED as early as possible, in C,
 * before the kernel is loaded.
 *
 * Append the body of custom_status_led_on() to your board's existing
 * board_early_init_f() (runs before relocation, minimal C env) or
 * board_late_init() (runs after relocation, env available) in
 * board/<vendor>/<board>/<board>.c. If the board file defines neither, add one
 * and select the matching CONFIG_BOARD_EARLY_INIT_F / CONFIG_BOARD_LATE_INIT.
 */

#include <common.h>
#include <env.h>
#include <asm/gpio.h>

#ifndef CONFIG_CUSTOM_STATUS_LED_GPIO
#define CONFIG_CUSTOM_STATUS_LED_GPIO 42
#endif

int custom_status_led_on(void)
{
	int gpio = CONFIG_CUSTOM_STATUS_LED_GPIO;
	int ret;

	ret = gpio_request(gpio, "status-led");
	if (ret && ret != -EBUSY)
		return ret;

	gpio_direction_output(gpio, 1);	/* LED on = "bootloader alive" */
	gpio_free(gpio);
	return 0;
}

/*
 * Example wiring if you are adding a fresh hook:
 *
 * int board_late_init(void)
 * {
 *         custom_status_led_on();
 *         env_set("boot_started", "1");
 *         return 0;
 * }
 */
