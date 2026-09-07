// SPDX-License-Identifier: MIT
//
// Decodes a raw YOLO output tensor into pixel-space detections and runs NMS.
// Supports the two common export layouts:
//   * YOLOv5 / v7  : [N, 5+C]   (cx,cy,w,h,obj, class scores...)
//   * YOLOv8 / v11  : [4+C, N]   (transposed, no objectness)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace edge_ai {

struct Detection {
    float x = 0, y = 0, w = 0, h = 0; // pixel space of the ORIGINAL frame
    float score = 0;
    int   class_id = 0;
};

struct LetterboxInfo {
    float scale = 1.f;   // resized / original
    int   pad_x = 0;     // left padding in the letterboxed image
    int   pad_y = 0;     // top padding
    int   src_w = 0;
    int   src_h = 0;
};

struct PostProcessConfig {
    int   num_classes    = 80;
    float conf_threshold = 0.25f;
    float iou_threshold  = 0.45f;
    int   max_detections = 300;
    bool  transposed     = false; // true for YOLOv8-style [4+C, N]
};

// `output` points at a contiguous float tensor of `count` elements as produced
// by TrtEngine::output_host(0). `rows`/`cols` describe its 2D shape.
std::vector<Detection> decode_yolo(const float* output, int rows, int cols,
                                   const LetterboxInfo& lb, const PostProcessConfig& cfg);

// Standard greedy IoU NMS, class-aware.
std::vector<Detection> nms(std::vector<Detection> dets, float iou_threshold, int max_out);

// COCO-80 label helper (returns "id:N" if out of range).
const char* coco_label(int id);

} // namespace edge_ai
