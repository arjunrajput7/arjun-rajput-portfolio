// SPDX-License-Identifier: MIT
//
// Programmatically constructed GStreamer capture pipeline:
//
//   v4l2src ! capsfilter ! tee name=t
//        t. ! queue ! videoconvert ! <H.264 encoder> ! h264parse ! appsink(enc)
//        t. ! queue ! videoconvert ! video/x-raw,NV12   ! appsink(raw)
//
// The `enc` appsink feeds the RTSP server; the `raw` appsink is the "secondary
// memory sink". Both branches run on their own streaming threads (that is what
// the `queue` elements are for) so a stall in one cannot block the other.
//
// Everything is built with gst_element_factory_make() + manual linking -- no
// gst_parse_launch, no shell.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "gstpipe/frame_queue.hpp"
#include "gstpipe/latency_tracker.hpp"

namespace gstpipe {

// An H.264 access unit pulled from the encoder branch.
struct EncodedFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t             pts_ns       = 0;   // running presentation timestamp
    std::uint64_t             capture_mono_ns = 0; // wall clock when appsink saw it
    bool                      keyframe     = false;
};

// A raw frame handed to the secondary sink (owned copy; simple + safe).
struct RawFrame {
    std::vector<std::uint8_t> data;
    int                       width  = 0;
    int                       height = 0;
    std::string               format;             // e.g. "NV12"
    std::uint64_t             pts_ns = 0;
};

enum class Encoder {
    Auto,     // pick v4l2h264enc if present, else x264enc
    V4L2H264, // hardware V4L2 M2M (Jetson / RPi / i.MX)
    X264,     // software fallback
};

struct PipelineConfig {
    std::string device    = "/dev/video0";
    int         width     = 1280;
    int         height    = 720;
    int         fps       = 30;
    int         bitrate_kbps = 4000;
    Encoder     encoder   = Encoder::Auto;
    std::size_t enc_queue_capacity = 60;
    std::size_t raw_queue_capacity = 8;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig cfg);
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Build + link every element. Throws std::runtime_error on failure.
    void build();

    // GST_STATE_PLAYING / GST_STATE_NULL.
    void start();
    void stop();

    // Encoded H.264 frames for the RTSP server to consume.
    FrameQueue<EncodedFrame>& encoded_queue() { return enc_q_; }
    // Raw frames for the secondary in-memory consumer.
    FrameQueue<RawFrame>&     raw_queue()     { return raw_q_; }

    const LatencyTracker& appsink_latency() const { return appsink_latency_; }

    // Negotiated caps string of the encoder branch (for RTSP SDP / logging).
    std::string encoder_caps() const;

    // Invoked from the GLib main loop on a bus ERROR; lets main() shut down.
    void set_error_handler(std::function<void(const std::string&)> cb) {
        on_error_ = std::move(cb);
    }

    GstElement* bin() const { return pipeline_; }

private:
    static GstFlowReturn on_new_encoded_sample(GstElement* sink, gpointer user);
    static GstFlowReturn on_new_raw_sample(GstElement* sink, gpointer user);
    static gboolean      on_bus_message(GstBus* bus, GstMessage* msg, gpointer user);

    GstElement* make(const char* factory, const char* name);
    void        select_encoder();
    void        link_tee_branch(GstElement* queue_head);

    PipelineConfig cfg_;
    std::string    encoder_factory_;

    GstElement* pipeline_   = nullptr;
    GstElement* source_     = nullptr;
    GstElement* tee_        = nullptr;
    GstElement* enc_sink_   = nullptr;
    GstElement* raw_sink_   = nullptr;
    guint       bus_watch_  = 0;

    FrameQueue<EncodedFrame> enc_q_;
    FrameQueue<RawFrame>     raw_q_;
    LatencyTracker           appsink_latency_;
    std::atomic<std::uint64_t> pts_counter_{0};

    std::function<void(const std::string&)> on_error_;
};

} // namespace gstpipe
