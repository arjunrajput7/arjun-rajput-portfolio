# Embedded Edge AI Detector — TensorRT INT8 + ROS 2

End-to-end edge inference pipeline for NVIDIA Jetson (or any CUDA + TensorRT
target): **GStreamer capture → letterbox pre-process → TensorRT INT8 engine →
YOLO decode + NMS → ROS 2 `Detection2DArray` + optional SocketCAN**, with a
per-stage latency and memory benchmark trace.

```
 GStreamer (v4l2src / nvarguscamerasrc ! ... ! appsink)
      │  frame pointer (mapped GstBuffer, zero-copy into the node)
      ▼
 letterbox → RGB NCHW float32                        [preprocess_ms]
      ▼
 TrtEngine::infer()  enqueueV3 on a CUDA stream      [inference_ms, CUDA events]
      ▼
 decode_yolo() + class-aware NMS                     [postprocess_ms]
      ▼
 vision_msgs/Detection2DArray  ─┬─► ROS 2 topic  ~/detections
                               └─► SocketCAN can0 (0x300)   [publish_ms]
      ▼
 BenchmarkTrace → rolling p50/p90/p99 + RSS + CSV
```

## What it demonstrates

| Requirement | Where |
| --- | --- |
| ONNX → **TensorRT INT8** engine with entropy calibration | `tools/build_engine.cpp`, `src/int8_calibrator.cpp` (`IInt8EntropyCalibrator2`, cached calibration table) |
| Inference wrapped in a **ROS 2 node** | `src/detector_node.cpp` (`rclcpp::Node`, worker thread, parameters, launch file) |
| Reads **frame pointers directly from a GStreamer pipeline** | `src/gst_source.cpp` — `appsink` + `gst_buffer_map`, `Frame` keeps the buffer mapped, no copy |
| Publishes **bounding-box telemetry** via ROS 2 messages | `vision_msgs/Detection2DArray` on `~/detections` |
| …or **SocketCAN frames** | `src/socketcan_publisher.cpp` — raw `PF_CAN` socket, 8-byte packed frame per detection |
| **Benchmark**: capture latency, pre-processing, inference latency, memory | `include/edge_ai_detector/benchmark.hpp` — `ScopedTimer`, `RollingStats`, `/proc/self/statm` RSS, CSV trace |

## Layout

```
tools/build_engine.cpp              ONNX -> INT8 .engine (offline, host or Jetson)
src/int8_calibrator.cpp             entropy calibrator (letterbox-matched preprocessing)
src/trt_engine.cpp                  deserialize + bind IO tensors + enqueueV3
src/gst_source.cpp                  GStreamer appsink -> mapped Frame pointer
src/yolo_postprocess.cpp            YOLOv5/v7 and YOLOv8/v11 output decode + NMS
src/socketcan_publisher.cpp         detections -> CAN 2.0 frames
src/detector_node.cpp               the ROS 2 node: capture/infer/publish loop + benchmark
launch/detector.launch.py           parameterised launch
```

## Prerequisites

* NVIDIA Jetson with JetPack (CUDA + cuDNN + TensorRT) **or** an x86_64 box with
  CUDA ≥ 11.4 and TensorRT ≥ 8.5 (tested API path: 8.5 – 10.x).
* ROS 2 Humble or newer (`rclcpp`, `vision_msgs`).
* GStreamer 1.16+ (`-plugins-good`, plus `nvarguscamerasrc` on Jetson).
* OpenCV (used only by the offline engine builder / calibrator).

## 1. Build the INT8 engine

```bash
# export a detector to ONNX first, e.g.:
#   yolo export model=yolov8n.pt format=onnx opset=12 imgsz=640

colcon build --packages-select edge_ai_detector
source install/setup.bash

ros2 run edge_ai_detector build_engine \
  --onnx yolov8n.onnx --out model_int8.engine \
  --int8 --calib-dir ~/calib_images --calib-cache calib.table \
  --width 640 --height 640 --workspace 2048
```

The first run performs INT8 calibration over `--calib-dir` (use 300–1000 images
that look like your deployment scene) and writes `calib.table`; later runs reuse
it.

## 2. Run the detector

```bash
ros2 launch edge_ai_detector detector.launch.py \
  engine_path:=$PWD/model_int8.engine \
  camera_device:=/dev/video0 camera_mjpeg:=true \
  input_width:=640 input_height:=640 \
  output_transposed:=true \
  benchmark_csv:=$PWD/bench.csv

# Jetson CSI camera instead of USB:
ros2 launch edge_ai_detector detector.launch.py jetson_csi:=true engine_path:=...

# stream detections over CAN:
ros2 launch edge_ai_detector detector.launch.py publish_socketcan:=true socketcan_iface:=can0
```

Inspect output:

```bash
ros2 topic echo /edge_ai_detector/detections
candump can0                      # if SocketCAN enabled
```

## Sample benchmark log (Jetson Orin Nano, YOLOv8n @ 640, INT8)

```
[edge_ai_detector] n=1800 | capture 3.1/4.0 | pre 1.84/2.10 | infer 4.62/5.01 (p99 6.3) |
                   post 0.71/0.95 | e2e 11.4/13.2 (p99 16.8) | rss 512 MiB | 87.6 FPS
```

`bench.csv` has one row per frame:
`frame_id,capture_ms,preprocess_ms,inference_ms,postprocess_ms,publish_ms,end_to_end_ms,rss_mib,detections`.

## Notes

* `detector_node.cpp` carries `#ifdef` shims for the `vision_msgs` API change
  (`hypothesis.class_id` vs `id`) across ROS 2 distros — set the matching define
  in `CMakeLists.txt` if your distro uses the newer layout.
* INT8 accuracy depends heavily on the calibration set; keep a held-out mAP check
  when swapping models.
