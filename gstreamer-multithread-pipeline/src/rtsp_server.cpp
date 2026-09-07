// SPDX-License-Identifier: MIT
#include "gstpipe/rtsp_server.hpp"

#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace gstpipe {

RtspServer::RtspServer(RtspConfig cfg, FrameQueue<EncodedFrame>& source)
    : cfg_(std::move(cfg)), source_(source) {}

RtspServer::~RtspServer() { shutdown(); }

void RtspServer::attach(GMainContext* ctx) {
    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server_, cfg_.service.c_str());

    mounts_  = gst_rtsp_server_get_mount_points(server_);
    factory_ = gst_rtsp_media_factory_new();

    // appsrc feeds an already-encoded byte-stream, so we only need parse + pay.
    const gchar* launch =
        "( appsrc name=vsrc is-live=true block=false format=time do-timestamp=false "
        "  ! h264parse config-interval=-1 "
        "  ! rtph264pay name=pay0 pt=96 config-interval=1 )";
    gst_rtsp_media_factory_set_launch(factory_, launch);

    // One encoder, many viewers: share a single pipeline instance.
    gst_rtsp_media_factory_set_shared(factory_, TRUE);
    gst_rtsp_media_factory_set_latency(factory_, 0);

    g_signal_connect(factory_, "media-configure", G_CALLBACK(on_media_configure), this);

    gst_rtsp_mount_points_add_factory(mounts_, cfg_.mount.c_str(), factory_);
    g_object_unref(mounts_);
    mounts_ = nullptr;

    source_id_ = gst_rtsp_server_attach(server_, ctx);
    if (source_id_ == 0) {
        g_printerr("[rtsp] failed to attach server to main context\n");
        return;
    }

    running_.store(true);
    feeder_ = std::thread(&RtspServer::feeder_loop, this);
    g_printerr("[rtsp] listening on %s\n", url().c_str());
}

void RtspServer::shutdown() {
    if (!running_.exchange(false)) return;
    source_.close();       // wake the feeder's blocking pop()
    if (feeder_.joinable()) feeder_.join();

    if (source_id_) {
        GSource* s = g_main_context_find_source_by_id(nullptr, source_id_);
        if (s) g_source_destroy(s);
        source_id_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(appsrc_mtx_);
        if (appsrc_) {
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
        }
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
}

std::string RtspServer::url(const std::string& host) const {
    return "rtsp://" + host + ":" + cfg_.service + cfg_.mount;
}

// --- gst-rtsp-server callbacks ------------------------------------------------

void RtspServer::on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer user) {
    auto* self       = static_cast<RtspServer*>(user);
    GstElement* bin  = gst_rtsp_media_get_element(media);
    GstElement* src  = gst_bin_get_by_name_recurse_up(GST_BIN(bin), "vsrc");
    if (!src) {
        g_printerr("[rtsp] media-configure: appsrc 'vsrc' not found\n");
        gst_object_unref(bin);
        return;
    }

    GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);

    g_object_set(src, "stream-type", 0 /* stream */, "max-bytes", guint64(2 * 1024 * 1024),
                 "min-latency", G_GINT64_CONSTANT(0), nullptr);
    g_signal_connect(src, "need-data", G_CALLBACK(on_need_data), self);
    g_signal_connect(src, "enough-data", G_CALLBACK(on_enough_data), self);

    {
        std::lock_guard<std::mutex> lk(self->appsrc_mtx_);
        if (self->appsrc_) gst_object_unref(self->appsrc_);
        self->appsrc_ = GST_ELEMENT(gst_object_ref(src));
    }
    self->feeding_.store(true);
    g_printerr("[rtsp] client connected -- media configured\n");

    gst_object_unref(src);
    gst_object_unref(bin);
}

void RtspServer::on_need_data(GstElement*, guint, gpointer user) {
    static_cast<RtspServer*>(user)->feeding_.store(true);
}

void RtspServer::on_enough_data(GstElement*, gpointer user) {
    static_cast<RtspServer*>(user)->feeding_.store(false);
}

// --- feeder thread ---------------------------------------------------------

void RtspServer::feeder_loop() {
    guint64 base_pts = GST_CLOCK_TIME_NONE;
    auto    t0       = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto item = source_.pop(); // blocks; returns nullopt when queue closed
        if (!item) break;

        // Respect appsrc backpressure: if the server has enough buffered, skip
        // ahead to the next keyframe rather than piling up latency.
        if (!feeding_.load()) {
            if (!item->keyframe) continue;
        }

        GstElement* src = nullptr;
        {
            std::lock_guard<std::mutex> lk(appsrc_mtx_);
            if (appsrc_) src = GST_ELEMENT(gst_object_ref(appsrc_));
        }
        if (!src) continue; // no client yet

        const std::uint64_t frame_bytes = item->data.size();
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
        gst_buffer_fill(buf, 0, item->data.data(), frame_bytes);

        if (base_pts == GST_CLOCK_TIME_NONE) base_pts = item->pts_ns;
        GstClockTime pts =
            (item->pts_ns >= base_pts) ? (item->pts_ns - base_pts)
                                       : GstClockTime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                          std::chrono::steady_clock::now() - t0)
                                                          .count());
        GST_BUFFER_PTS(buf)      = pts;
        GST_BUFFER_DTS(buf)      = pts;
        GST_BUFFER_DURATION(buf) = GST_SECOND / std::max(1, cfg_.fps);
        if (!item->keyframe) GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

        GstFlowReturn ret = GST_FLOW_OK;
        g_signal_emit_by_name(src, "push-buffer", buf, &ret);
        gst_buffer_unref(buf);
        gst_object_unref(src);

        if (ret == GST_FLOW_OK) {
            served_.fetch_add(1);
        } else if (ret == GST_FLOW_FLUSHING) {
            // client went away between checks -- ignore
        } else {
            g_printerr("[rtsp] push-buffer returned %s\n", gst_flow_get_name(ret));
        }
    }
    g_printerr("[rtsp] feeder thread exiting (served=%llu)\n",
               (unsigned long long)served_.load());
}

} // namespace gstpipe
