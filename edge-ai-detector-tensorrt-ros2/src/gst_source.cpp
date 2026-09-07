// SPDX-License-Identifier: MIT
#include "edge_ai_detector/gst_source.hpp"

#include <gst/app/gstappsink.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace edge_ai {

namespace {
std::uint64_t now_mono_ns() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}
} // namespace

// ---- Frame ---------------------------------------------------------------

Frame::Frame(GstSample* sample) : sample_(sample) {
    capture_mono_ns_ = now_mono_ns();
    GstBuffer* buf   = gst_sample_get_buffer(sample_);
    GstCaps*   caps  = gst_sample_get_caps(sample_);
    if (!buf || !caps) return;

    GstStructure* s = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(s, "width", &width_);
    gst_structure_get_int(s, "height", &height_);
    pts_ = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) : 0;

    if (gst_buffer_map(buf, &map_, GST_MAP_READ)) {
        data_   = map_.data;
        size_   = map_.size;
        stride_ = height_ > 0 ? int(size_ / height_) : 0;
    }
}

Frame::Frame(Frame&& o) noexcept { *this = std::move(o); }

Frame& Frame::operator=(Frame&& o) noexcept {
    if (this != &o) {
        release();
        sample_ = o.sample_;
        map_    = o.map_;
        data_   = o.data_;
        size_   = o.size_;
        width_  = o.width_;
        height_ = o.height_;
        stride_ = o.stride_;
        pts_    = o.pts_;
        capture_mono_ns_ = o.capture_mono_ns_;
        o.sample_ = nullptr;
        o.data_   = nullptr;
    }
    return *this;
}

Frame::~Frame() { release(); }

void Frame::release() {
    if (sample_) {
        if (data_) {
            GstBuffer* buf = gst_sample_get_buffer(sample_);
            if (buf) gst_buffer_unmap(buf, &map_);
        }
        gst_sample_unref(sample_);
        sample_ = nullptr;
        data_   = nullptr;
    }
}

// ---- GstSource --------------------------------------------------------

GstSource::GstSource(GstSourceConfig cfg) : cfg_(std::move(cfg)) {}

GstSource::~GstSource() { stop(); }

std::string GstSource::build_launch() const {
    std::ostringstream os;
    if (cfg_.jetson_csi) {
        os << "nvarguscamerasrc ! "
           << "video/x-raw(memory:NVMM),width=" << cfg_.width << ",height=" << cfg_.height
           << ",framerate=" << cfg_.fps << "/1 ! nvvidconv ! ";
    } else if (cfg_.mjpeg) {
        os << "v4l2src device=" << cfg_.device << " io-mode=2 ! "
           << "image/jpeg,width=" << cfg_.width << ",height=" << cfg_.height
           << ",framerate=" << cfg_.fps << "/1 ! jpegdec ! ";
    } else {
        os << "v4l2src device=" << cfg_.device << " io-mode=2 ! "
           << "video/x-raw,width=" << cfg_.width << ",height=" << cfg_.height
           << ",framerate=" << cfg_.fps << "/1 ! ";
    }
    os << "videoconvert ! video/x-raw,format=" << cfg_.format << " ! "
       << "appsink name=sink sync=false max-buffers=2 drop=true";
    return os.str();
}

void GstSource::start() {
    GError* err = nullptr;
    const std::string launch = build_launch();
    std::fprintf(stderr, "[gst-src] %s\n", launch.c_str());

    // gst_parse_launch is used ONLY for the fixed capture front-end; the
    // inference graph and all runtime control stay in C++.
    pipeline_ = gst_parse_launch(launch.c_str(), &err);
    if (!pipeline_ || err) {
        std::string m = err ? err->message : "unknown";
        if (err) g_error_free(err);
        throw std::runtime_error("gst_parse_launch failed: " + m);
    }

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) throw std::runtime_error("appsink 'sink' not found");
    gst_app_sink_set_drop(GST_APP_SINK(appsink_), TRUE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(appsink_), 2);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("capture pipeline failed to reach PLAYING");
}

void GstSource::stop() {
    if (appsink_) {
        gst_object_unref(appsink_);
        appsink_ = nullptr;
    }
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

Frame GstSource::next(int timeout_ms) {
    if (!appsink_) return {};
    GstSample* sample = gst_app_sink_try_pull_sample(
        GST_APP_SINK(appsink_), GstClockTime(timeout_ms) * GST_MSECOND);
    if (!sample) {
        if (gst_app_sink_is_eos(GST_APP_SINK(appsink_)))
            std::fprintf(stderr, "[gst-src] EOS\n");
        else
            ++drops_;
        return {};
    }
    ++pulled_;
    return Frame(sample);
}

} // namespace edge_ai
