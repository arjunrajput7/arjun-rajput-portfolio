# U-Boot Fast Bootloader Customization & GPIO Status Indicator

A minimal U-Boot customization set: a **custom `diag` command**
(`U_BOOT_CMD`), **early GPIO status-LED init in C**, and a tuned **boot
environment** for a fast, quiet handoff to Linux.

Target: any ARM/ARM64 board U-Boot supports (Raspberry Pi, BeagleBone, STM32MP,
i.MX) or QEMU. Verified flow below uses `qemu_arm64` — no hardware needed.

## Contents

```
src/cmd_diag.c          custom 'diag' command: info / mem walk / led control
src/status_led_init.c   custom_status_led_on() for board_early_init_f / board_late_init
src/integration.diff    the 2 tiny cmd/Makefile + cmd/Kconfig hunks
env/uEnv.txt            bootargs / bootcmd / bootdelay=0 fast-boot environment
```

| Requirement | Where |
| --- | --- |
| Custom U-Boot command via `U_BOOT_CMD` | `src/cmd_diag.c` — `diag info`, `diag mem`, `diag led on/off/blink` |
| Early GPIO init (LED on before kernel) | `src/status_led_init.c` — `gpio_request` + `gpio_direction_output` in a board init hook |
| Custom `bootargs` / `bootcmd` passed to Linux | `env/uEnv.txt` — `quiet loglevel=3`, `bootdelay=0`, `bootcmd=run diagcmd; run mmcboot` |

## Build (QEMU arm64, quick path)

```bash
git clone --depth=1 https://source.denx.de/u-boot/u-boot.git
cd u-boot

# 1. add the custom command
cp ../uboot-fast-boot-gpio/src/cmd_diag.c cmd/
#    then apply the two hunks from src/integration.diff to cmd/Makefile + cmd/Kconfig
patch -p1 < ../uboot-fast-boot-gpio/src/integration.diff

# 2. configure + build
make qemu_arm64_defconfig
./scripts/config --enable CMD_DIAG
make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu-

# 3. run it
qemu-system-aarch64 -M virt -cpu cortex-a57 -nographic -bios u-boot.bin
```

At the U-Boot prompt:

```
=> diag info
== board diagnostics ==
soc         : ...
DRAM size   : 128 MiB
bootargs    : (unset)
status LED  : gpio 42
=> diag mem
diag: mem walk ok (0 errors)
=> diag led blink 3
```

## Use on real hardware

1. **Command** — same copy + `integration.diff` steps against your board's
   `<board>_defconfig`; set `CUSTOM_STATUS_LED_GPIO` to your LED's number
   (`./scripts/config --set-val CUSTOM_STATUS_LED_GPIO 16`).
2. **Early LED** — paste the body of `custom_status_led_on()` into your board's
   `board_early_init_f()` (pre-relocation) or `board_late_init()` in
   `board/<vendor>/<board>/<board>.c`, and make sure
   `CONFIG_BOARD_EARLY_INIT_F` / `CONFIG_BOARD_LATE_INIT` is set.
3. **Environment** — copy `env/uEnv.txt` to the FAT boot partition (RPi /
   BeagleBone auto-read it), or from U-Boot:
   ```
   fatload mmc 0:1 $loadaddr uEnv.txt
   env import -t $loadaddr $filesize
   saveenv
   ```
   Adjust `root=`, load addresses, and the DTB filename for your board.

## Fast-boot notes

- `bootdelay=0` + no autoboot prompt removes the interactive pause.
- `quiet loglevel=3 consoleblank=0` cuts kernel console spew (a real time saver
  on slow UARTs).
- `bootcmd` goes straight to `mmcboot` — no `dhcp`, no `usb start`, no
  `pxe`/`bootflow` scan.
- The LED comes on in `board_*_init` (bootloader alive) and `preboot` blinks it
  twice right before `booti`, giving a visible "kernel handoff" marker.
