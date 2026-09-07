// SPDX-License-Identifier: MIT
//
// GStreamer capture source that hands the inference node a *pointer* to the
// decoded frame (mapped GstBuffer memory), not a copy. The pipeline is built
// with the C API:
//
//   v4l2src ! image/jpeg,... ! jpegdec        (USB webcam / MJPEG)   \
//   v4l2src ! video/x-raw,...                  (raw sensor)           |-> videoconvert ! video/x-raw,BGR ! appsink
//   nvarguscamerasrc ! ... ! nvvidconv         (Jetson CSI)          /
//
// A Frame keeps the GstSample mapped for its lifetime; destroy it (or call
// release()) to return the buffer to the pool.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <gst/gst.h>

namespace edge_ai {

class GstSource;

class Frame {
public:
    Frame() = default;
    Frame(GstSample* sample);
    Frame(Frame&& o) noexcept;
    Frame& operator=(Frame&& o) noexcept;
    ~Frame();
    Frame(const Frame&)            = delete;
    Frame& operator=(const Frame&) = delete;

    bool            valid() const { return sample_ != nullptr && data_ != nullptr; }
    const uint8_t*  data()  const { return data_; }
    std::size_t     size()  const { return size_; }
    int             width() const { return width_; }
    int             height() const { return height_; }
    int             stride() const { return stride_; }          // bytes per row
    std::uint64_t   pts_ns() const { return pts_; }
    std::uint64_t   capture_mono_ns() const { return capture_mono_ns_; }

    void release();

private:
    GstSample*   sample_ = nullptr;
    GstMapInfo   map_{};
    const uint8_t* data_ = nullptr;
    std::size_t  size_   = 0;
    int          width_ = 0, height_ = 0, stride_ = 0;
    std::uint64_t pts_ = 0;
    std::uint64_t capture_mono_ns_ = 0;
};

struct GstSourceConfig {
    std::string device   = "/dev/video0";
    int         width    = 1280;
    int         height   = 720;
    int         fps      = 30;
    bool        mjpeg    = true;   // decode MJPEG (typical USB webcam)
    bool        jetson_csi = false; // use nvarguscamerasrc + nvvidconv
    std::string format   = "BGR";  // appsink output pixel format
};

class GstSource {
public:
    explicit GstSource(GstSourceConfig cfg);
    ~GstSource();

    void start();
    void stop();

    // Blocking pull with timeout. Returns an invalid Frame on timeout / EOS.
    Frame next(int timeout_ms = 1000);

    std::uint64_t frames_pulled() const { return pulled_; }
    std::uint64_t drops()        const { return drops_; }

private:
    std::string build_launch() const;

    GstSourceConfig cfg_;
    GstElement*     pipeline_ = nullptr;
    GstElement*     appsink_  = nullptr;
    std::uint64_t   pulled_ = 0;
    std::uint64_t   drops_  = 0;
};

} // namespace edge_ai
