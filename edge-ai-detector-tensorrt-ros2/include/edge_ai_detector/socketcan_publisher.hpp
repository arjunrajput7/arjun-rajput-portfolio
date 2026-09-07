// SPDX-License-Identifier: MIT
//
// Publishes detection telemetry as classic CAN 2.0 frames over a SocketCAN
// interface (e.g. can0 on a Jetson, or vcan0 for desktop testing).
//
// Frame layout (8 data bytes, big-endian fields):
//   byte 0      : detection index within this frame set
//   byte 1      : total detections in this set
//   byte 2      : class id
//   byte 3      : confidence * 255
//   bytes 4-5   : center x, normalised to 0..65535 of frame width
//   bytes 6-7   : center y, normalised to 0..65535 of frame height
// CAN ID = base_id (default 0x300).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edge_ai_detector/yolo_postprocess.hpp"

namespace edge_ai {

class SocketCanPublisher {
public:
    SocketCanPublisher(std::string iface, std::uint32_t base_id = 0x300);
    ~SocketCanPublisher();

    bool open();                                  // returns false if iface is down
    bool publish(const std::vector<Detection>& dets, int frame_w, int frame_h);
    bool is_open() const { return fd_ >= 0; }

private:
    std::string   iface_;
    std::uint32_t base_id_;
    int           fd_ = -1;
};

} // namespace edge_ai
