// SPDX-License-Identifier: GPL-2.0+
/*
 * cmd_diag.c - custom U-Boot board diagnostics command.
 *
 * Drop this file into U-Boot's cmd/ directory and wire it into cmd/Makefile
 * and cmd/Kconfig (see this project's README). It adds:
 *
 *   diag info    - SoC / board / DRAM / reset-cause summary
 *   diag mem     - short non-destructive DRAM read/write walk
 *   diag led on|off|blink <n>  - toggle the board status LED
 *
 * Usage from the U-Boot prompt or from bootcmd:  run diagcmd  /  diag info
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <linux/delay.h>

DECLARE_GLOBAL_DATA_PTR;

/* Board status LED. Override from the environment: setenv status_led_gpio 42 */
#ifndef CONFIG_CUSTOM_STATUS_LED_GPIO
#define CONFIG_CUSTOM_STATUS_LED_GPIO 42
#endif

static int status_led_gpio(void)
{
	const char *s = env_get("status_led_gpio");

	return s ? (int)simple_strtoul(s, NULL, 10)
		 : CONFIG_CUSTOM_STATUS_LED_GPIO;
}

static int led_set(int on)
{
	int gpio = status_led_gpio();
	int ret;

	ret = gpio_request(gpio, "status-led");
	if (ret && ret != -EBUSY) {
		printf("diag: gpio_request(%d) failed: %d\n", gpio, ret);
		return CMD_RET_FAILURE;
	}
	gpio_direction_output(gpio, on ? 1 : 0);
	gpio_free(gpio);
	return CMD_RET_SUCCESS;
}

static int do_diag_info(void)
{
	printf("== board diagnostics ==\n");
#ifdef CONFIG_SYS_BOARD
	printf("board       : %s\n", CONFIG_SYS_BOARD);
#endif
#ifdef CONFIG_SYS_SOC
	printf("soc         : %s\n", CONFIG_SYS_SOC);
#endif
	printf("U-Boot      : %s\n", PLAIN_VERSION);
	printf("DRAM size   : %llu MiB\n",
	       (unsigned long long)(gd->ram_size >> 20));
	printf("relocaddr   : 0x%08lx\n", gd->relocaddr);
	printf("boot_params : 0x%08lx\n", (ulong)gd->bd->bi_boot_params);
	printf("bootargs    : %s\n", env_get("bootargs") ?: "(unset)");
	printf("status LED  : gpio %d\n", status_led_gpio());
	return CMD_RET_SUCCESS;
}

static int do_diag_mem(void)
{
	volatile ulong *base = (ulong *)CONFIG_SYS_SDRAM_BASE;
	const int words = 256;
	int i, errors = 0;
	ulong saved[256];

	printf("diag: DRAM walk at %p (%d words)\n", base, words);
	for (i = 0; i < words; i++) {
		saved[i] = base[i];
		base[i] = 0xA5A50000UL | i;
	}
	for (i = 0; i < words; i++) {
		if (base[i] != (0xA5A50000UL | (ulong)i))
			errors++;
		base[i] = saved[i];	/* restore -- non-destructive */
	}
	printf("diag: mem walk %s (%d errors)\n", errors ? "FAILED" : "ok", errors);
	return errors ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_diag(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "info"))
		return do_diag_info();

	if (!strcmp(argv[1], "mem"))
		return do_diag_mem();

	if (!strcmp(argv[1], "led")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		if (!strcmp(argv[2], "on"))
			return led_set(1);
		if (!strcmp(argv[2], "off"))
			return led_set(0);
		if (!strcmp(argv[2], "blink")) {
			int n = (argc > 3) ? (int)simple_strtoul(argv[3], NULL, 10) : 3;
			while (n-- > 0) {
				led_set(1); mdelay(120);
				led_set(0); mdelay(120);
			}
			return CMD_RET_SUCCESS;
		}
		return CMD_RET_USAGE;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	diag, 4, 0, do_diag,
	"board bring-up diagnostics",
	"info                - print SoC / board / DRAM / reset summary\n"
	"diag mem                 - non-destructive DRAM read/write walk\n"
	"diag led on|off|blink [n] - drive the board status LED"
);
