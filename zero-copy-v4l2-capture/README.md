# Zero-Copy V4L2 Video Capture Engine

A standalone C++17 Linux application that captures video directly from a V4L2
device (`/dev/videoN`) using **raw `ioctl()` calls only** — no `libv4l`, no
`cv::VideoCapture`, no GStreamer. Frames are delivered from driver-owned buffers
mapped into userspace with `mmap()`, so no pixel data is ever copied between
kernel and userspace.

Works with any UVC device: a USB webcam, an integrated laptop camera, or a MIPI
CSI sensor exposed through a V4L2 driver.

## What it demonstrates

| Area | Implementation |
| --- | --- |
| Device discovery & capability negotiation | `VIDIOC_QUERYCAP`, checks for `V4L2_CAP_VIDEO_CAPTURE` / `V4L2_CAP_STREAMING` |
| Format negotiation | `VIDIOC_S_FMT` with driver-substitution handling (resolution / FourCC fallback) |
| Streaming I/O with `mmap` | `VIDIOC_REQBUFS` → `VIDIOC_QUERYBUF` → `mmap()` → `VIDIOC_QBUF` |
| Zero-copy DMABUF export | `VIDIOC_EXPBUF` turns each mapped buffer into a shareable `dmabuf` FD (`--dmabuf`) |
| Streaming state machine | `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`, safe re-start |
| Frame pump | `poll()` + `VIDIOC_DQBUF` / `VIDIOC_QBUF`, `EAGAIN` / `EINTR` handling |
| Frame-drop detection | tracks `v4l2_buffer.sequence` gaps |
| Corrupt-frame detection | honours `V4L2_BUF_FLAG_ERROR` |
| Latency measurement | driver buffer timestamp → `CLOCK_MONOTONIC` at `DQBUF` |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires a Linux kernel with `linux/videodev2.h` (any modern distro) and a C++17
compiler. No third-party dependencies.

## Run

```bash
# 300 frames of 1280x720 YUYV from the default camera
./build/zcv4l2 --device /dev/video0 --width 1280 --height 720 --format YUYV --frames 300

# Export each buffer as a dmabuf fd (zero-copy hand-off target)
./build/zcv4l2 --dmabuf

# Dump raw frames to disk for inspection (e.g. ffplay -f rawvideo ...)
./build/zcv4l2 --format YUYV --raw-out capture.raw --frames 60
```

### CLI

| Flag | Default | Meaning |
| --- | --- | --- |
| `--device` | `/dev/video0` | V4L2 node |
| `--width` / `--height` | `1280` / `720` | requested resolution |
| `--format` | `YUYV` | FourCC (`YUYV`, `MJPG`, `NV12`, …) |
| `--buffers` | `4` | number of streaming buffers to request |
| `--frames` | `300` | frames to grab (`0` = run until Ctrl-C) |
| `--dmabuf` | off | `VIDIOC_EXPBUF` each buffer |
| `--raw-out` | — | write raw frames to this file |

## Sample output

```
[zcv4l2] driver=uvcvideo card=Integrated Camera bus=usb-0000:00:14.0-8
[zcv4l2] format 1280x720 YUYV, 2560 bytes/line, 1843200 byte image
[zcv4l2] streaming started (4 buffers, MMAP)
[zcv4l2] frame 0  seq=0  latency=12.44ms  avg=12.44ms  dropped=0  dmabuf_fd=-1
[zcv4l2] frame 30 seq=30 latency=9.81ms   avg=10.12ms  dropped=0  dmabuf_fd=-1
...
==== capture summary ====
frames captured   : 300
frames dropped    : 0
frames errored    : 0
effective FPS     : 30.0
avg DQBUF latency : 10.06 ms
```

## Design notes

* **Non-blocking fd + `poll()`** rather than a blocking read loop, so the app
  stays responsive to signals and can multiplex other fds later.
* **`EINTR` retry** on every `ioctl()` via a small `xioctl()` wrapper.
* **Driver substitution is respected** — if the camera can't do the exact
  resolution/format requested, the negotiated values from `VIDIOC_S_FMT` are used
  everywhere downstream instead of failing.
* **Buffer ownership is explicit**: `next()` hands out a `Frame` that points into
  a driver buffer; the caller must `release()` it to re-`QBUF`. This mirrors how
  a real pipeline would hold a frame only as long as needed.

## Layout

```
include/zcv4l2/v4l2_capture.hpp   public API
include/zcv4l2/logging.hpp        errno helpers
src/v4l2_capture.cpp              ioctl state machine
src/main.cpp                      CLI + capture loop + stats
```
