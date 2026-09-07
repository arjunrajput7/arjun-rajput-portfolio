#!/usr/bin/env bash
# Poll the bmp280_simple sysfs interface once a second.
set -euo pipefail

DEV=$(ls -d /sys/bus/i2c/devices/*/temperature 2>/dev/null | head -n1 | xargs -r dirname || true)
if [[ -z "${DEV}" ]]; then
	echo "bmp280_simple device not found. Is the module loaded and the overlay applied?" >&2
	exit 1
fi

echo "reading ${DEV}"
while true; do
	t_mc=$(cat "${DEV}/temperature")   # millidegrees C
	p_pa=$(cat "${DEV}/pressure")      # pascals
	printf '%(%H:%M:%S)T  %.2f C  %.2f hPa\n' -1 \
		"$(echo "scale=2; ${t_mc}/1000" | bc)" \
		"$(echo "scale=2; ${p_pa}/100"  | bc)"
	sleep 1
done
