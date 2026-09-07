#!/usr/bin/env bash
# Create (or tear down) a virtual CAN interface.
#   sudo ./setup_vcan.sh up     [vcan0]
#   sudo ./setup_vcan.sh down   [vcan0]
set -euo pipefail

ACTION="${1:-up}"
IFACE="${2:-vcan0}"

case "$ACTION" in
  up)
    modprobe vcan
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
      ip link add dev "$IFACE" type vcan
    fi
    ip link set up "$IFACE"
    ip -details -brief link show "$IFACE"
    ;;
  down)
    ip link set down "$IFACE" || true
    ip link delete "$IFACE" type vcan
    ;;
  *)
    echo "usage: $0 {up|down} [iface]" >&2
    exit 2
    ;;
esac
