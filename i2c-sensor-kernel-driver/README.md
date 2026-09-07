# Custom Linux I2C Sensor Kernel Driver & Device Tree Overlay

An out-of-tree Linux **I2C client driver** for the Bosch **BMP280** temperature /
pressure sensor, plus the **device tree overlay** that binds it. Written from
scratch in kernel C — no `libi2c`, no user-space sensor library, no IIO.

Target: Raspberry Pi (any model with the 40-pin header) or QEMU `virt` with an
emulated I2C sensor. Works on BeagleBone with a different overlay target.

## What it shows

- Linux **platform driver model**: `i2c_driver` with `probe`/`remove`, `id_table`,
  and an `of_match_table`, registered via `module_i2c_driver()`.
- **Datasheet work**: reads the factory calibration block (`0x88..0x9F`) and
  applies the BMP280 datasheet fixed-point compensation to raw ADC words.
- **Sysfs interface**: `dev_groups` exposes two read-only attributes returning
  kernel-standard units:
  - `temperature` — millidegrees Celsius
  - `pressure` — pascals
- **Device tree overlay**: declares the chip, its bus address (`0x76`), and the
  bus speed (400 kHz), compiled with `dtc -@`.

## Wiring (Raspberry Pi)

| BMP280 pin | Pi header |
| --- | --- |
| VCC | 3V3 (pin 1) |
| GND | GND (pin 6) |
| SDA | GPIO2 / SDA1 (pin 3) |
| SCL | GPIO3 / SCL1 (pin 5) |
| SDO | GND → address `0x76` (to 3V3 → `0x77`) |

Enable the bus: `sudo raspi-config` → Interface Options → I2C, or add
`dtparam=i2c_arm=on` to `/boot/config.txt`. Confirm with
`i2cdetect -y 1` (you should see `76`).

## Build

```bash
# against the running kernel (install raspberrypi-kernel-headers first)
make

# against a cross-compiled tree
make KDIR=~/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

Produces `bmp280_simple.ko` and `dts/bmp280-overlay.dtbo`.

> Kernel ≥ 6.3 uses the single-argument `.probe`. On 5.3–6.2 rename it to
> `.probe_new` (one-line change, noted in the source).

## Load

```bash
sudo dtoverlay dts/bmp280-overlay.dtbo      # runtime overlay
sudo insmod bmp280_simple.ko
dmesg | tail                                # "BMP280 ready at 0x76 ..."

DEV=$(dirname $(ls /sys/bus/i2c/devices/*/temperature | head -1))
cat $DEV/temperature   # e.g. 23140  -> 23.14 C
cat $DEV/pressure      # e.g. 100730 -> 1007.30 hPa

./scripts/read_sensor.sh                    # 1 Hz pretty-printed loop
```

Persistent load: copy `bmp280-overlay.dtbo` to `/boot/overlays/`, add
`dtoverlay=bmp280-overlay` to `/boot/config.txt`, and `sudo modprobe bmp280_simple`
(after `make install`).

## Unload

```bash
sudo rmmod bmp280_simple
sudo dtoverlay -r bmp280-overlay
```

## Files

```
bmp280_simple.c            the driver (probe/remove, calibration, compensation, sysfs)
dts/bmp280-overlay.dts     device tree overlay source
Makefile                   kbuild + dtc
scripts/read_sensor.sh     user-space poller
```
