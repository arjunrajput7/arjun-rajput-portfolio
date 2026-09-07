# meta-custom-bsp — Custom Yocto Layer for a Headless Embedded System

A small BSP layer that turns stock Poky into a **headless, stripped-down Linux
image** with a custom kernel config, a cross-compiled C++ app, and a systemd
service that starts it on boot.

Tested against **kirkstone** and **scarthgap**. Machines: `raspberrypi4-64`,
`raspberrypi5`, or `qemuarm64`.

## What's in the layer

```
meta-custom-bsp/
├── conf/layer.conf
├── recipes-kernel/linux/
│   ├── linux-raspberrypi_%.bbappend        # append a kernel config fragment
│   └── files/netfilter.cfg                 # CONFIG_NF_TABLES, NAT, conntrack, ...
├── recipes-apps/telemetry-app/
│   ├── telemetry-app_1.0.bb                # cross-compile + package the app
│   └── files/
│       ├── telemetry_app.cpp               # C++17 daemon (uptime / load / CPU temp)
│       └── telemetry-app.service           # systemd unit, auto-enabled
└── recipes-core/images/
    └── custom-headless-image.bb            # minimal no-GUI image, size report task
```

| Requirement | Where |
| --- | --- |
| Modify kernel config via `.bbappend` | `linux-raspberrypi_%.bbappend` + `netfilter.cfg` fragment |
| Cross-compile a C++ app into the image | `telemetry-app_1.0.bb` (`do_compile` with `${CXX}`, `do_install`) |
| systemd service that auto-starts the app | `telemetry-app.service`, `inherit systemd` + `SYSTEMD_AUTO_ENABLE` |
| Minimal image, no GUI, trimmed deps | `custom-headless-image.bb` (`DISTRO_FEATURES:remove`, no package-management, docs/locale stripped) |

## Build

```bash
# 1. get poky
git clone -b scarthgap git://git.yoctoproject.org/poky.git
# (for RPi machines also: git clone -b scarthgap git://git.yoctoproject.org/meta-raspberrypi)

# 2. init the build env
cd poky
source oe-init-build-env build

# 3. add the layers
bitbake-layers add-layer ../../meta-raspberrypi          # RPi machines only
bitbake-layers add-layer /path/to/meta-custom-bsp

# 4. pick a machine + systemd
echo 'MACHINE = "raspberrypi4-64"'                 >> conf/local.conf
echo 'INIT_MANAGER = "systemd"'                    >> conf/local.conf
echo 'DISTRO_FEATURES:append = " systemd"'         >> conf/local.conf

# 5. build
bitbake custom-headless-image
```

`qemuarm64` needs no `meta-raspberrypi` layer and no SD card — run it with
`runqemu qemuarm64 nographic`.

## Verify on target

```bash
uname -a
zcat /proc/config.gz | grep NF_TABLES          # =y, from the fragment
systemctl status telemetry-app                  # active (running), enabled
journalctl -u telemetry-app -f                  # tick=... uptime_s=... cpu_c=...
du -sh --exclude=/proc --exclude=/sys /         # rootfs footprint
```

The build also prints `custom-headless-image rootfs size: NN.N MiB` at the end
(`do_report_size` task).

## Notes

- The kernel `.bbappend` targets `linux-raspberrypi`. For a QEMU-only build,
  rename it to `linux-yocto_%.bbappend` — the fragment mechanism is identical.
- Getting under 100 MB reliably: keep `INIT_MANAGER = "systemd"` but avoid
  pulling `openssh`; `ssh-server-dropbear` (used here) plus `packagegroup-core-boot`
  lands around 40–60 MB depending on kernel modules.
