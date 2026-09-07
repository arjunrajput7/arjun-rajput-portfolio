# Arjun Rajput — Embedded Software Portfolio

**Embedded software developer — 5+ years in Embedded Linux, BSP, and device
drivers.** Seven self-contained projects that trace the stack from the bootloader
and kernel UAPI up through a custom Linux distribution, multimedia middleware, and
edge-AI deployment. Each one builds, runs, and reports its own results.

> **Short on time?** Start with
> [`i2c-sensor-kernel-driver/bmp280_simple.c`](./i2c-sensor-kernel-driver/bmp280_simple.c)
> (a from-scratch I2C client driver) and
> [`zero-copy-v4l2-capture/src/v4l2_capture.cpp`](./zero-copy-v4l2-capture/src/v4l2_capture.cpp)
> (raw V4L2 `ioctl` streaming).

## Projects

| # | Project | Stack | Proves |
| --- | --- | --- | --- |
| 1 | [U-Boot Bootloader Customization & GPIO Status LED](./uboot-fast-boot-gpio) | C, U-Boot | custom `U_BOOT_CMD` diagnostics command, early GPIO LED init in C before the kernel, tuned `bootargs` / `bootcmd` / `bootdelay=0` fast boot |
| 2 | [Custom I2C Sensor Kernel Driver & Device Tree Overlay](./i2c-sensor-kernel-driver) | C, Linux kernel, I2C client model, `dtc` | out-of-tree `i2c_driver` (probe/remove, OF match table), BMP280 datasheet calibration + fixed-point compensation, sysfs interface, runtime `.dtbo` overlay — no `libi2c`, no IIO |
| 3 | [Custom Yocto Layer — `meta-custom-bsp`](./yocto-meta-custom-bsp) | Yocto / BitBake, systemd | kernel config via `.bbappend` fragment, cross-compiled C++ app recipe, auto-enabled `systemd` service, minimal headless image with a size-report task |
| 4 | [Zero-Copy V4L2 Video Capture Engine](./zero-copy-v4l2-capture) | C++17, Linux V4L2 UAPI, `mmap`, DMABUF | raw `ioctl()` device bring-up, kernel↔user memory mapping, streaming state machine, frame-drop / latency instrumentation — no `libv4l`, no OpenCV |
| 5 | [Multi-Threaded GStreamer Pipeline (C++ wrapper)](./gstreamer-multithread-pipeline) | C++17, GStreamer C API, gst-rtsp-server | programmatic pipeline construction, `tee` branching, hardware H.264 (V4L2 M2M), RTSP restream, thread-safe bounded queues, latency tracking |
| 6 | [Embedded Edge AI Detector (TensorRT + ROS 2)](./edge-ai-detector-tensorrt-ros2) | C++17, TensorRT INT8, CUDA, ROS 2, GStreamer, SocketCAN | ONNX→INT8 engine build with calibration, `enqueueV3` inference, ROS 2 node consuming GStreamer frame pointers, `Detection2DArray` / CAN telemetry, per-stage benchmarking |
| 7 | [Virtual CAN & SocketCAN Telemetry Bridge](./vcan-socketcan-telemetry-bridge) | C, SocketCAN | `PF_CAN` raw sockets, `CAN_RAW_FILTER` kernel filtering, DBC-matched frame encode/decode, threshold alerts, `can-utils` test harness |

Each project has its own `README.md` with build instructions, a CLI/parameter
reference, and sample output.

## Skills demonstrated

| Area | Specifics |
| --- | --- |
| Languages & build | C, C++17, CMake, BitBake, Make, Python (ROS 2 launch) |
| Kernel & boot | out-of-tree kernel modules, I2C client / platform driver model, device tree overlays (`dtc`), U-Boot commands & board init (`U_BOOT_CMD`) |
| Linux systems | V4L2 UAPI & `ioctl(2)`, `mmap(2)`, DMA-BUF export, `poll(2)`, sysfs, SocketCAN (`CAN_RAW_FILTER`), POSIX threads, signals, `/proc` |
| Distro / BSP | Yocto layers, `.bbappend` kernel config fragments, recipe authoring, systemd unit packaging, image size reduction |
| Multimedia | GStreamer C API, `gst-rtsp-server`, `appsrc` / `appsink`, `tee`, V4L2 M2M hardware H.264 / H.265, RTSP / RTP |
| Edge AI | TensorRT (INT8 entropy calibration, `enqueueV3`), CUDA streams & events, ONNX, YOLO pre/post-processing, NMS |
| Robotics / automotive | ROS 2 (`rclcpp`, `vision_msgs`), CAN 2.0 framing, DBC |
| Cross-cutting | zero-copy buffer handling, explicit resource lifetime (RAII / `QBUF` return), latency-percentile instrumentation |

## Themes across the projects

- **Down to the register** — datasheet calibration math, raw `ioctl()` sequences,
  bootloader board init; third-party wrappers avoided on purpose.
- **Zero / minimal copy** — `mmap`'d V4L2 buffers, DMABUF export, mapped
  `GstBuffer` pointers passed straight into pre-processing.
- **Explicit lifetime & threading** — buffers are handed back deliberately
  (`release()` / `QBUF`); branch threads decoupled by bounded drop-oldest queues.
- **Measure everything** — latency percentiles, drop counts, memory, CSV traces.
- **Hardware-aware** — V4L2 M2M encoders, TensorRT INT8 on Jetson, SocketCAN, GPIO.

## Platforms

| Projects | Target |
| --- | --- |
| 1–3 | ARM/ARM64 board (Raspberry Pi, BeagleBone) or QEMU; a Linux host with kernel headers / BitBake / a U-Boot tree |
| 4–5 | mainline Linux with V4L2 + GStreamer 1.16+ |
| 6 | NVIDIA Jetson (JetPack) or an x86_64 host with CUDA ≥ 11.4 and TensorRT ≥ 8.5 |
| 7 | any Linux PC — `vcan` needs no hardware |

The projects are not intended to build on macOS or Windows.

## Building

Each project is independent; build whichever you need.

```bash
# 1  U-Boot customization  — copy files into a U-Boot tree (see its README)
# 2  I2C kernel driver
make -C i2c-sensor-kernel-driver
# 3  Yocto layer           — bitbake-layers add-layer .../meta-custom-bsp && bitbake custom-headless-image
# 4  V4L2 capture
cmake -S zero-copy-v4l2-capture -B zero-copy-v4l2-capture/build && cmake --build zero-copy-v4l2-capture/build -j
# 5  GStreamer pipeline
cmake -S gstreamer-multithread-pipeline -B gstreamer-multithread-pipeline/build && cmake --build gstreamer-multithread-pipeline/build -j
# 6  Edge AI detector (ROS 2 package)
cd edge-ai-detector-tensorrt-ros2 && colcon build
# 7  SocketCAN bridge
make -C vcan-socketcan-telemetry-bridge
```

Per-project prerequisites are listed in each README.

## Contact

- Email: arjun.rajput1525@gmail.com
- GitHub: https://github.com/arjunrajput7


## License

Userspace projects (4–7) are released under the [MIT License](./LICENSE). The
kernel module (project 2) is GPL-2.0 and the U-Boot changes (project 1) are
GPL-2.0+, as required by those code bases.
