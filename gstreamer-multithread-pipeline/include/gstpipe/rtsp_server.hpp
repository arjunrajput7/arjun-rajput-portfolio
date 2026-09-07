// SPDX-License-Identifier: MIT
//
// Minimal RTSP server built on gst-rtsp-server. The media factory exposes an
// `appsrc` that is fed, frame by frame, from the Pipeline's encoded FrameQueue by
// a dedicated feeder thread. Backpressure is handled through the appsrc
// need-data / enough-data signals so we never buffer unbounded data in the
// server when no client is pulling.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "gstpipe/frame_queue.hpp"
#include "gstpipe/pipeline.hpp"

namespace gstpipe {

struct RtspConfig {
    std::string service = "8554";       // TCP port
    std::string mount   = "/live";      // rtsp://<host>:8554/live
    int         fps     = 30;           // used to synthesise PTS if needed
};

class RtspServer {
public:
    RtspServer(RtspConfig cfg, FrameQueue<EncodedFrame>& source);
    ~RtspServer();

    RtspServer(const RtspServer&)            = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Attach to the given GMainContext/GMainLoop-driven context and start
    // listening. Must be called from the thread that runs the GMainLoop.
    void attach(GMainContext* ctx = nullptr);

    // Stop the feeder thread and release the server.
    void shutdown();

    std::string url(const std::string& host = "127.0.0.1") const;

    std::uint64_t frames_served() const { return served_.load(); }

private:
    // gst-rtsp-server callbacks (static -> dispatch to instance).
    static void on_media_configure(GstRTSPMediaFactory* f, GstRTSPMedia* media, gpointer user);
    static void on_need_data(GstElement* appsrc, guint length, gpointer user);
    static void on_enough_data(GstElement* appsrc, gpointer user);

    void feeder_loop();

    RtspConfig                 cfg_;
    FrameQueue<EncodedFrame>&  source_;

    GstRTSPServer*         server_  = nullptr;
    GstRTSPMountPoints*    mounts_  = nullptr;
    GstRTSPMediaFactory*  factory_ = nullptr;
    guint                 source_id_ = 0;

    GstElement*           appsrc_    = nullptr;  // set on media-configure
    std::mutex            appsrc_mtx_;
    std::atomic<bool>     feeding_{false};       // need-data gate
    std::atomic<bool>     running_{false};
    std::thread           feeder_;
    std::atomic<std::uint64_t> served_{0};
};

} // namespace gstpipe
