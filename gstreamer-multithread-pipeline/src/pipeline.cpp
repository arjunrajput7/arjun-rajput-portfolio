// SPDX-License-Identifier: MIT
#include "gstpipe/pipeline.hpp"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace gstpipe {

namespace {

std::uint64_t now_mono_ns() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

bool factory_exists(const char* name) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (f) {
        gst_object_unref(f);
        return true;
    }
    return false;
}

} // namespace

Pipeline::Pipeline(PipelineConfig cfg)
    : cfg_(std::move(cfg)),
      enc_q_(cfg_.enc_queue_capacity),
      raw_q_(cfg_.raw_queue_capacity) {}

Pipeline::~Pipeline() {
    stop();
    if (bus_watch_) g_source_remove(bus_watch_);
    if (pipeline_) gst_object_unref(pipeline_);
}

GstElement* Pipeline::make(const char* factory, const char* name) {
    GstElement* e = gst_element_factory_make(factory, name);
    if (!e) throw std::runtime_error(std::string("gst_element_factory_make failed: ") + factory);
    return e;
}

void Pipeline::select_encoder() {
    switch (cfg_.encoder) {
        case Encoder::V4L2H264: encoder_factory_ = "v4l2h264enc"; break;
        case Encoder::X264:     encoder_factory_ = "x264enc";      break;
        case Encoder::Auto:
            encoder_factory_ = factory_exists("v4l2h264enc") ? "v4l2h264enc" : "x264enc";
            break;
    }
    if (!factory_exists(encoder_factory_.c_str()))
        throw std::runtime_error("encoder element not available: " + encoder_factory_);
    g_printerr("[pipeline] using encoder: %s\n", encoder_factory_.c_str());
}

void Pipeline::build() {
    select_encoder();

    pipeline_ = gst_pipeline_new("capture-pipeline");
    if (!pipeline_) throw std::runtime_error("gst_pipeline_new failed");

    source_          = make("v4l2src", "cam");
    GstElement* caps = make("capsfilter", "srccaps");
    tee_             = make("tee", "t");

    g_object_set(source_, "device", cfg_.device.c_str(), "io-mode", 2 /* mmap */, nullptr);

    GstCaps* src_caps = gst_caps_new_simple(
        "video/x-raw", "width", G_TYPE_INT, cfg_.width, "height", G_TYPE_INT, cfg_.height,
        "framerate", GST_TYPE_FRACTION, cfg_.fps, 1, nullptr);
    g_object_set(caps, "caps", src_caps, nullptr);
    gst_caps_unref(src_caps);

    // ---- encoder branch ------------------------------------------------------
    GstElement* eq   = make("queue", "enc_q");
    GstElement* ecvt = make("videoconvert", "enc_cvt");
    GstElement* enc  = make(encoder_factory_.c_str(), "enc");
    GstElement* eprs = make("h264parse", "enc_parse");
    enc_sink_        = make("appsink", "enc_sink");

    g_object_set(eq, "leaky", 2 /* downstream */, "max-size-buffers", 8,
                 "max-size-time", guint64(0), "max-size-bytes", 0, nullptr);

    if (encoder_factory_ == "x264enc") {
        g_object_set(enc, "tune", 0x00000004 /* zerolatency */, "speed-preset", 1 /* ultrafast */,
                     "key-int-max", cfg_.fps * 2, "bitrate", cfg_.bitrate_kbps, nullptr);
    } else { // v4l2h264enc -- options live in the "extra-controls" structure
        GstStructure* ctrls = gst_structure_new(
            "controls", "video_bitrate", G_TYPE_INT, cfg_.bitrate_kbps * 1000,
            "h264_i_frame_period", G_TYPE_INT, cfg_.fps * 2, nullptr);
        g_object_set(enc, "extra-controls", ctrls, nullptr);
        gst_structure_free(ctrls);
    }

    g_object_set(eprs, "config-interval", -1 /* send SPS/PPS with every IDR */, nullptr);
    g_object_set(enc_sink_, "emit-signals", TRUE, "sync", FALSE, "drop", FALSE,
                 "max-buffers", 8, nullptr);
    GstCaps* enc_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    gst_app_sink_set_caps(GST_APP_SINK(enc_sink_), enc_caps);
    gst_caps_unref(enc_caps);
    g_signal_connect(enc_sink_, "new-sample", G_CALLBACK(on_new_encoded_sample), this);

    // ---- raw / secondary memory branch ------------------------------------
    GstElement* rq   = make("queue", "raw_q");
    GstElement* rcvt = make("videoconvert", "raw_cvt");
    GstElement* rcap = make("capsfilter", "raw_caps");
    raw_sink_        = make("appsink", "raw_sink");

    g_object_set(rq, "leaky", 2, "max-size-buffers", 4, "max-size-time", guint64(0),
                 "max-size-bytes", 0, nullptr);
    GstCaps* raw_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", nullptr);
    g_object_set(rcap, "caps", raw_caps, nullptr);
    gst_caps_unref(raw_caps);
    g_object_set(raw_sink_, "emit-signals", TRUE, "sync", FALSE, "drop", TRUE, "max-buffers", 2,
                 nullptr);
    g_signal_connect(raw_sink_, "new-sample", G_CALLBACK(on_new_raw_sample), this);

    // ---- assemble ----------------------------------------------------------
    gst_bin_add_many(GST_BIN(pipeline_), source_, caps, tee_, eq, ecvt, enc, eprs, enc_sink_, rq,
                     rcvt, rcap, raw_sink_, nullptr);

    if (!gst_element_link_many(source_, caps, tee_, nullptr))
        throw std::runtime_error("link v4l2src -> capsfilter -> tee failed");
    if (!gst_element_link_many(eq, ecvt, enc, eprs, enc_sink_, nullptr))
        throw std::runtime_error("link encoder branch failed");
    if (!gst_element_link_many(rq, rcvt, rcap, raw_sink_, nullptr))
        throw std::runtime_error("link raw branch failed");

    // tee request pads -> the head queue of each branch
    link_tee_branch(eq);
    link_tee_branch(rq);

    GstBus* bus = gst_element_get_bus(pipeline_);
    bus_watch_  = gst_bus_add_watch(bus, on_bus_message, this);
    gst_object_unref(bus);
}

void Pipeline::link_tee_branch(GstElement* queue_head) {
    // gst_element_request_pad_simple() is the 1.20+ name; get_request_pad works
    // across 1.16 - 1.24.
    GstPad* tee_pad = gst_element_get_request_pad(tee_, "src_%u");
    GstPad* q_pad   = gst_element_get_static_pad(queue_head, "sink");
    if (!tee_pad || !q_pad || gst_pad_link(tee_pad, q_pad) != GST_PAD_LINK_OK)
        throw std::runtime_error("failed to link a tee src pad to a branch queue");
    gst_object_unref(q_pad);
    gst_object_unref(tee_pad);
}

void Pipeline::start() {
    GstStateChangeReturn r = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("pipeline failed to reach PLAYING");
    g_printerr("[pipeline] PLAYING\n");
}

void Pipeline::stop() {
    if (!pipeline_) return;
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    enc_q_.close();
    raw_q_.close();
}

std::string Pipeline::encoder_caps() const {
    if (!enc_sink_) return {};
    GstPad*  pad  = gst_element_get_static_pad(enc_sink_, "sink");
    GstCaps* caps = pad ? gst_pad_get_current_caps(pad) : nullptr;
    std::string out;
    if (caps) {
        gchar* s = gst_caps_to_string(caps);
        out      = s ? s : "";
        g_free(s);
        gst_caps_unref(caps);
    }
    if (pad) gst_object_unref(pad);
    return out;
}

GstFlowReturn Pipeline::on_new_encoded_sample(GstElement* sink, gpointer user) {
    auto* self       = static_cast<Pipeline*>(user);
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        EncodedFrame f;
        f.data.assign(map.data, map.data + map.size);
        f.keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
        f.pts_ns   = self->pts_counter_.fetch_add(GST_SECOND / std::max(1, self->cfg_.fps));
        if (GST_BUFFER_PTS_IS_VALID(buf)) f.pts_ns = GST_BUFFER_PTS(buf);
        f.capture_mono_ns = now_mono_ns();

        // Latency: pipeline running-time of this buffer vs. wall clock now.
        GstClock* clk = gst_element_get_clock(self->pipeline_);
        if (clk && GST_BUFFER_PTS_IS_VALID(buf)) {
            GstClockTime base = gst_element_get_base_time(self->pipeline_);
            GstClockTime now  = gst_clock_get_time(clk);
            if (now > base) {
                double ms = (double(now - base) - double(GST_BUFFER_PTS(buf))) / 1e6;
                if (ms > 0 && ms < 5000) self->appsink_latency_.add(ms);
            }
        }
        if (clk) gst_object_unref(clk);

        gst_buffer_unmap(buf, &map);
        self->enc_q_.push(std::move(f));
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

GstFlowReturn Pipeline::on_new_raw_sample(GstElement* sink, gpointer user) {
    auto* self       = static_cast<Pipeline*>(user);
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buf  = gst_sample_get_buffer(sample);
    GstCaps*   caps = gst_sample_get_caps(sample);
    GstMapInfo map;
    if (buf && caps && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        GstStructure* s = gst_caps_get_structure(caps, 0);
        RawFrame f;
        gst_structure_get_int(s, "width", &f.width);
        gst_structure_get_int(s, "height", &f.height);
        const gchar* fmt = gst_structure_get_string(s, "format");
        f.format         = fmt ? fmt : "NV12";
        f.pts_ns         = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) : 0;
        f.data.assign(map.data, map.data + map.size);
        gst_buffer_unmap(buf, &map);
        self->raw_q_.push(std::move(f)); // drop-oldest keeps this branch bounded
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

gboolean Pipeline::on_bus_message(GstBus*, GstMessage* msg, gpointer user) {
    auto* self = static_cast<Pipeline*>(user);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::string text = err ? err->message : "unknown";
            g_printerr("[pipeline][ERROR] %s (%s)\n", text.c_str(), dbg ? dbg : "");
            if (err) g_error_free(err);
            g_free(dbg);
            if (self->on_error_) self->on_error_(text);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            g_printerr("[pipeline][warn] %s (%s)\n", err ? err->message : "?", dbg ? dbg : "");
            if (err) g_error_free(err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            g_printerr("[pipeline] EOS\n");
            if (self->on_error_) self->on_error_("end-of-stream");
            break;
        case GST_MESSAGE_QOS: {
            guint64 dropped = 0, processed = 0;
            gst_message_parse_qos_stats(msg, nullptr, &processed, &dropped);
            if (dropped)
                g_printerr("[pipeline][qos] %s dropped=%" G_GUINT64_FORMAT "\n",
                           GST_OBJECT_NAME(msg->src), dropped);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

} // namespace gstpipe
