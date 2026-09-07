# High-Throughput Multi-Threaded GStreamer Pipeline (C++ Wrapper)

A C++17 application that builds a **dynamic GStreamer pipeline programmatically**
through the GStreamer C API (`gst/gst.h`) — no `gst-launch`, no shell strings for
the capture graph. A single camera feed is split with `tee` into two independent,
separately-threaded branches:

```
                              ┌─ queue ─ videoconvert ─ H.264 enc ─ h264parse ─ appsink(enc) ──► RTSP feeder ─► gst-rtsp-server (appsrc)
 v4l2src ─ capsfilter ─ tee ──┤
                              └─ queue ─ videoconvert ─ NV12 caps  ───────────── appsink(raw) ──► secondary in-memory sink
```

## What it demonstrates

| Area | Implementation |
| --- | --- |
| Programmatic pipeline construction | `gst_element_factory_make` + `gst_element_link_many` + `tee` request pads (`src_%u`) |
| Dynamic stream branching | `tee` → two `queue`-headed branches, linked at runtime |
| Hardware encode | `v4l2h264enc` (V4L2 M2M on Jetson / RPi / i.MX), auto-fallback to `x264enc` |
| RTSP network streaming | `gst-rtsp-server` media factory with an `appsrc` fed from the encoder branch |
| Secondary memory sink | second `appsink` delivering raw NV12 frames to a consumer thread |
| Thread-safe queue management | `FrameQueue<T>`: bounded, mutex + condvar, **drop-oldest** overflow policy, push/pop/drop counters |
| Backpressure | `appsrc` `need-data` / `enough-data` gate the feeder; skips to next keyframe when the server is full |
| Latency tracking | `LatencyTracker`: rolling min / mean / p95 / max over pipeline running-time vs. wall clock at `appsink` |
| Robust bus handling | `ERROR` / `WARNING` / `EOS` / `QOS` messages on a `gst_bus_add_watch` |

## Threading model

| Thread | Owner | Job |
| --- | --- | --- |
| GLib main loop | app | bus messages, RTSP server socket I/O |
| encoder branch streaming thread | GStreamer | runs `tee → enc → appsink(enc)` |
| raw branch streaming thread | GStreamer | runs `tee → convert → appsink(raw)` |
| RTSP feeder | app | `FrameQueue<EncodedFrame>::pop()` → `appsrc` push-buffer |
| raw consumer | app | `FrameQueue<RawFrame>::pop()` → stats / SHM / recording |
| reporter | app | 1 Hz telemetry line |

The two `queue` elements are what decouple the branch threads — a stall in the
RTSP path cannot back-pressure the raw path and vice versa.

## Build

```bash
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
                 libgstrtspserver-1.0-dev gstreamer1.0-plugins-good \
                 gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Jetson, `nvv4l2h264enc` is also available — swap the factory name in
`Pipeline::select_encoder()` if you prefer the NVIDIA element over the mainline
`v4l2h264enc`.

## Run

```bash
# software encode, restream on rtsp://<host>:8554/live
./build/gstpipe --device /dev/video0 --width 1280 --height 720 --fps 30 --bitrate 4000

# force hardware V4L2 M2M encoder
./build/gstpipe --encoder v4l2 --bitrate 6000 --rtsp-port 8554 --mount /cam0
```

Play it back:

```bash
gst-play-1.0 rtsp://127.0.0.1:8554/live
# or
ffplay -fflags nobuffer -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

## Sample telemetry

```
[pipeline] using encoder: v4l2h264enc
[pipeline] PLAYING
[rtsp] listening on rtsp://127.0.0.1:8554/live
[rtsp] client connected -- media configured
[stat] enc_q(size=1 push=451 pop=450 drop=0) raw_q(size=0 drop=7) rtsp_served=449 lat_ms(mean=18.4 p95=27.1 max=41.0 n=451)
[raw-sink] 120 frames  253 MiB  1280x720 NV12  (acc=98)
```

## Layout

```
include/gstpipe/frame_queue.hpp      bounded thread-safe queue (drop-oldest)
include/gstpipe/latency_tracker.hpp  rolling min/mean/p95/max
include/gstpipe/pipeline.hpp         pipeline API + EncodedFrame / RawFrame
include/gstpipe/rtsp_server.hpp      gst-rtsp-server wrapper
src/pipeline.cpp                     factory_make + link + tee + appsink callbacks
src/rtsp_server.cpp                  media factory + appsrc feeder thread
src/main.cpp                         glue, consumers, telemetry, signal handling
```
