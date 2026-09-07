// SPDX-License-Identifier: MIT
#include "edge_ai_detector/yolo_postprocess.hpp"

#include <algorithm>
#include <cmath>

namespace edge_ai {

namespace {

inline float iou(const Detection& a, const Detection& b) {
    const float ax2 = a.x + a.w, ay2 = a.y + a.h;
    const float bx2 = b.x + b.w, by2 = b.y + b.h;
    const float ix1 = std::max(a.x, b.x), iy1 = std::max(a.y, b.y);
    const float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
    const float iw = std::max(0.f, ix2 - ix1), ih = std::max(0.f, iy2 - iy1);
    const float inter = iw * ih;
    const float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

// Map a box from letterboxed input space back to original-frame pixels.
Detection unletterbox(float cx, float cy, float bw, float bh, const LetterboxInfo& lb) {
    float x = (cx - bw * 0.5f - lb.pad_x) / lb.scale;
    float y = (cy - bh * 0.5f - lb.pad_y) / lb.scale;
    float w = bw / lb.scale;
    float h = bh / lb.scale;
    x = std::clamp(x, 0.f, float(lb.src_w - 1));
    y = std::clamp(y, 0.f, float(lb.src_h - 1));
    w = std::clamp(w, 0.f, float(lb.src_w) - x);
    h = std::clamp(h, 0.f, float(lb.src_h) - y);
    return Detection{x, y, w, h, 0.f, 0};
}

} // namespace

std::vector<Detection> nms(std::vector<Detection> dets, float iou_threshold, int max_out) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> keep;
    std::vector<char> removed(dets.size(), 0);
    for (std::size_t i = 0; i < dets.size(); ++i) {
        if (removed[i]) continue;
        keep.push_back(dets[i]);
        if (int(keep.size()) >= max_out) break;
        for (std::size_t j = i + 1; j < dets.size(); ++j) {
            if (removed[j]) continue;
            if (dets[j].class_id == dets[i].class_id && iou(dets[i], dets[j]) > iou_threshold)
                removed[j] = 1;
        }
    }
    return keep;
}

std::vector<Detection> decode_yolo(const float* output, int rows, int cols, const LetterboxInfo& lb,
                                   const PostProcessConfig& cfg) {
    std::vector<Detection> raw;
    raw.reserve(256);

    // Normalise to a row-per-candidate view.
    // Non-transposed: rows = num_boxes, cols = 5+C  (or 4+C without objectness)
    // Transposed:     rows = 4+C, cols = num_boxes  -> read column-major
    const int num_boxes = cfg.transposed ? cols : rows;
    const int stride     = cfg.transposed ? cols : cols; // element gap within a "row"
    const int attrs      = cfg.transposed ? rows : cols;
    const bool has_obj   = (attrs == cfg.num_classes + 5);
    const int  cls_off   = has_obj ? 5 : 4;

    auto at = [&](int box, int attr) -> float {
        return cfg.transposed ? output[attr * stride + box] : output[box * stride + attr];
    };

    for (int i = 0; i < num_boxes; ++i) {
        const float cx = at(i, 0), cy = at(i, 1), bw = at(i, 2), bh = at(i, 3);
        const float obj = has_obj ? at(i, 4) : 1.f;
        if (obj < cfg.conf_threshold * 0.5f && has_obj) continue;

        int   best_c = -1;
        float best_s = 0.f;
        for (int c = 0; c < cfg.num_classes; ++c) {
            const float s = at(i, cls_off + c);
            if (s > best_s) {
                best_s = s;
                best_c = c;
            }
        }
        const float score = best_s * obj;
        if (best_c < 0 || score < cfg.conf_threshold) continue;

        Detection d = unletterbox(cx, cy, bw, bh, lb);
        d.score     = score;
        d.class_id  = best_c;
        raw.push_back(d);
    }

    return nms(std::move(raw), cfg.iou_threshold, cfg.max_detections);
}

const char* coco_label(int id) {
    static const char* k[] = {
        "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
        "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog",
        "horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag",
        "tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat",
        "baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup",
        "fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot",
        "hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table",
        "toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
        "toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
        "hair drier","toothbrush"};
    if (id < 0 || id >= int(sizeof(k) / sizeof(k[0]))) return "unknown";
    return k[id];
}

} // namespace edge_ai
