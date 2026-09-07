# Virtual CAN (vcan) & SocketCAN Industrial Telemetry Bridge

Two small C programs that talk automotive-style CAN over a Linux **virtual CAN**
interface — no hardware required. A transmitter simulates a vehicle ECU; a
receiver installs kernel raw-socket filters, decodes frames per a DBC, and raises
threshold alerts.

## What it shows

- **SocketCAN** raw sockets (`PF_CAN` / `CAN_RAW`), `bind()` to an interface via
  `SIOCGIFINDEX`.
- **Kernel-side filtering** with `setsockopt(SOL_CAN_RAW, CAN_RAW_FILTER, ...)` —
  only the two message IDs are delivered to user space.
- **Frame packing/unpacking**: big-endian multibyte signals, scale/offset
  conversions matching `dbc/telemetry.dbc`.
- **Deterministic timing** in the transmitter with `clock_nanosleep(TIMER_ABSTIME)`.
- Verification with **can-utils** (`candump`, `cansend`).

## Messages (`dbc/telemetry.dbc`)

| ID | Name | Signals |
| --- | --- | --- |
| `0x0C0` | `ENGINE_DATA` | `EngineRPM` (0.25 rpm), `CoolantTemp` (1 °C, −40 offset), `ThrottlePos` (0.4 %), `MIL` bit |
| `0x0D0` | `VEHICLE_DATA` | `VehicleSpeed` (0.01 km/h), `Odometer` (1 m, 24-bit) |

Alert thresholds in the receiver: RPM > 6500, coolant > 110 °C, speed > 180 km/h.

## Build

```bash
make            # -> ./can_tx  ./can_rx
```

## Run

```bash
# 1. bring up vcan0 (needs root once)
sudo scripts/setup_vcan.sh up            # modprobe vcan; ip link add ...

# 2. terminal A - receiver
./can_rx vcan0

# 3. terminal B - simulator
./can_tx vcan0 --hz 10                    # normal drive cycle
./can_tx vcan0 --redline                  # force RPM/coolant past thresholds
```

Receiver output:

```
can_rx: listening on vcan0 (filtered: 0x0C0, 0x0D0)
ENGINE   rpm=  2470  ect=  82 C  thr= 40.0 %  MIL=0
VEHICLE  speed= 29.40 km/h  odo=12 m  (d=+0.82)
ENGINE   rpm=  9100  ect= 128 C  thr= 78.0 %  MIL=1
  !! ALERT: RPM 9100 over redline 6500
  !! ALERT: coolant 128 C over 110 C
```

## Verify with can-utils

```bash
sudo apt install can-utils

candump -t z vcan0                        # raw frame view
cansend vcan0 0C0#7D00AA9601000000        # manual ENGINE_DATA: RPM 8000, ECT 130C
./scripts/test.sh                         # scripted: simulate + candump + cansend + decode
```

Load the DBC in any CAN tool (e.g. `python-can` + `cantools`) to cross-check the
decode:

```bash
python3 -c "import cantools,can; db=cantools.database.load_file('dbc/telemetry.dbc'); \
bus=can.Bus('vcan0',bustype='socketcan'); \
[print(db.decode_message(m.arbitration_id,m.data)) for m in [bus.recv() for _ in range(5)]]"
```

## Files

```
src/can_common.h     IDs, signal scaling, endian helpers, thresholds
src/can_tx.c          ECU simulator (synthetic drive cycle, --redline)
src/can_rx.c          filtered receiver + DBC-matched decode + alerts
dbc/telemetry.dbc     message/signal database
scripts/setup_vcan.sh vcan0 up/down
scripts/test.sh       can-utils smoke test
```
