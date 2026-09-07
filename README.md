# Arjun Rajput — Embedded Software Portfolio

**Embedded software developer, 5+ years in Embedded Linux, BSP, and device
drivers.** Comfortable from the kernel UAPI up to multimedia middleware and
edge-AI deployment. This repository collects three self-contained projects that
show that range end to end.

| # | Project | Stack | Proves |
| --- | --- | --- | --- |
| 1 | [Zero-Copy V4L2 Video Capture Engine](./zero-copy-v4l2-capture) | C++17, Linux V4L2 UAPI, `mmap`, DMABUF | raw `ioctl()` device bring-up, kernel↔user memory mapping, streaming state machine, frame-drop / latency instrumentation — no `libv4l`, no OpenCV |
| 2 | [Multi-Threaded GStreamer Pipeline (C++ wrapper)](./gstreamer-multithread-pipeline) | C++17, GStreamer C API, gst-rtsp-server | programmatic pipeline construction, `tee` branching, hardware H.264 (V4L2 M2M), RTSP restream, thread-safe bounded queues, latency tracking |
| 3 | [Embedded Edge AI Detector (TensorRT + ROS 2)](./edge-ai-detector-tensorrt-ros2) | C++17, TensorRT INT8, CUDA, ROS 2, GStreamer, SocketCAN | ONNX→INT8 engine build with calibration, `enqueueV3` inference, ROS 2 node consuming GStreamer frame pointers, `Detection2DArray` / CAN telemetry, per-stage benchmarking |

Each project has its own `README.md` with build instructions, a CLI/parameter
reference, and sample output.

## Themes across the three

- **Zero / minimal copy** — `mmap`'d V4L2 buffers, DMABUF export, mapped
  `GstBuffer` pointers handed straight to pre-processing.
- **Explicit lifetime & threading** — buffer ownership is handed back
  deliberately (`release()` / `QBUF`); branch threads are decoupled by bounded
  drop-oldest queues.
- **Measure everything** — every pipeline reports latency percentiles, drop
  counts, and (project 3) memory, with a CSV trace for offline analysis.
- **Hardware-aware** — V4L2 M2M encoders, TensorRT INT8 on Jetson, SocketCAN.

## Building

The projects are independent; build whichever you need.

```bash
# Project 1
cmake -S zero-copy-v4l2-capture -B zero-copy-v4l2-capture/build && cmake --build zero-copy-v4l2-capture/build -j

# Project 2
cmake -S gstreamer-multithread-pipeline -B gstreamer-multithread-pipeline/build && cmake --build gstreamer-multithread-pipeline/build -j

# Project 3 (ROS 2 workspace)
cd edge-ai-detector-tensorrt-ros2 && colcon build
```

Per-project prerequisites are listed in each README. Projects 1–2 target
mainline Linux; project 3 targets NVIDIA Jetson / a CUDA+TensorRT host.

## Contact

- Email: `arjun.rajput@example.com` <!-- replace with the address you want public -->
- GitHub: https://github.com/<your-username>
- LinkedIn: https://www.linkedin.com/in/<your-handle>

## License

All three projects are released under the [MIT License](./LICENSE).
