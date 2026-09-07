#!/usr/bin/env bash
# Smoke test using can-utils: start the simulator, capture with candump, and
# inject one hand-crafted "redline" frame with cansend.
#
#   ./scripts/test.sh            # needs: sudo scripts/setup_vcan.sh up
set -euo pipefail

IFACE="${1:-vcan0}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

command -v candump  >/dev/null || { echo "install can-utils"; exit 1; }
[[ -x "$HERE/can_tx" ]] || make -C "$HERE"

echo "== 3 s capture of the live simulator =="
"$HERE/can_tx" "$IFACE" --hz 20 & TX=$!
trap 'kill $TX 2>/dev/null || true' EXIT
sleep 0.3
timeout 3 candump -t z "$IFACE" || true

echo
echo "== inject a manual ENGINE_DATA frame: RPM 8000 (0x7D00/0.25=0x7D00), ECT 130C =="
# RPM raw = 8000/0.25 = 32000 = 0x7D00 ; ECT raw = 130 - (-40) = 170 = 0xAA
cansend "$IFACE" 0C0#7D00AA9601000000
sleep 0.2

echo
echo "== run can_rx for 2 s to see it decode + alert =="
timeout 2 "$HERE/can_rx" "$IFACE" || true
