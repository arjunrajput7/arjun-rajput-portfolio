// SPDX-License-Identifier: MIT
//
// Lightweight pipeline instrumentation: per-stage timers, rolling percentiles,
// process RSS sampling, and an optional CSV trace. Zero dependencies beyond the
// standard library + /proc.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace edge_ai {

class RollingStats {
public:
    explicit RollingStats(std::size_t window = 300) : window_(window) { buf_.reserve(window); }

    void add(double v) {
        if (buf_.size() < window_) buf_.push_back(v);
        else buf_[cursor_] = v;
        cursor_ = (cursor_ + 1) % window_;
        ++n_;
        sum_ += v;
    }

    struct Snap { std::uint64_t n; double mean, p50, p90, p99, max; };

    Snap snap() const {
        Snap s{n_, 0, 0, 0, 0, 0};
        if (buf_.empty()) return s;
        std::vector<double> v(buf_);
        std::sort(v.begin(), v.end());
        auto q = [&](double p) { return v[std::min(v.size() - 1, std::size_t(v.size() * p))]; };
        s.mean = sum_ / double(n_);
        s.p50 = q(0.50);
        s.p90 = q(0.90);
        s.p99 = q(0.99);
        s.max = v.back();
        return s;
    }

private:
    std::size_t window_, cursor_ = 0;
    std::uint64_t n_ = 0;
    double sum_ = 0;
    std::vector<double> buf_;
};

class ScopedTimer {
public:
    explicit ScopedTimer(double& out_ms) : out_(out_ms), t0_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        out_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
                   .count();
    }

private:
    double& out_;
    std::chrono::steady_clock::time_point t0_;
};

// Resident set size in MiB, read from /proc/self/statm (Linux).
inline double rss_mib() {
    std::ifstream f("/proc/self/statm");
    long size_pages = 0, resident_pages = 0;
    if (f >> size_pages >> resident_pages) {
        const long page = 4096; // sysconf(_SC_PAGESIZE) on all supported targets
        return double(resident_pages) * page / (1024.0 * 1024.0);
    }
    return 0.0;
}

// One row of the per-frame benchmark trace.
struct FrameMetrics {
    std::uint64_t frame_id      = 0;
    double capture_latency_ms   = 0; // sensor PTS / pull -> handed to node
    double preprocess_ms        = 0; // letterbox + NCHW + normalise
    double inference_ms         = 0; // GPU time from CUDA events
    double postprocess_ms       = 0; // decode + NMS
    double publish_ms           = 0; // ROS 2 / SocketCAN publish
    double end_to_end_ms        = 0;
    double rss_mib              = 0;
    int    detections           = 0;
};

class BenchmarkTrace {
public:
    void open(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        csv_.open(path, std::ios::trunc);
        if (csv_)
            csv_ << "frame_id,capture_ms,preprocess_ms,inference_ms,postprocess_ms,publish_ms,"
                    "end_to_end_ms,rss_mib,detections\n";
    }

    void record(const FrameMetrics& m) {
        std::lock_guard<std::mutex> lk(m_);
        cap_.add(m.capture_latency_ms);
        pre_.add(m.preprocess_ms);
        inf_.add(m.inference_ms);
        post_.add(m.postprocess_ms);
        e2e_.add(m.end_to_end_ms);
        last_rss_ = m.rss_mib;
        if (csv_) {
            csv_ << m.frame_id << ',' << m.capture_latency_ms << ',' << m.preprocess_ms << ','
                 << m.inference_ms << ',' << m.postprocess_ms << ',' << m.publish_ms << ','
                 << m.end_to_end_ms << ',' << m.rss_mib << ',' << m.detections << '\n';
        }
    }

    std::string summary() const {
        std::lock_guard<std::mutex> lk(m_);
        auto c = cap_.snap(); auto p = pre_.snap(); auto i = inf_.snap();
        auto q = post_.snap(); auto e = e2e_.snap();
        char b[512];
        std::snprintf(b, sizeof(b),
            "n=%llu | capture %.1f/%.1f | pre %.2f/%.2f | infer %.2f/%.2f (p99 %.2f) | "
            "post %.2f/%.2f | e2e %.1f/%.1f (p99 %.1f) | rss %.0f MiB | %.1f FPS",
            (unsigned long long)e.n, c.mean, c.p90, p.mean, p.p90, i.mean, i.p90, i.p99,
            q.mean, q.p90, e.mean, e.p90, e.p99, last_rss_,
            e.mean > 0 ? 1000.0 / e.mean : 0.0);
        return b;
    }

private:
    mutable std::mutex m_;
    RollingStats cap_, pre_, inf_, post_, e2e_;
    double last_rss_ = 0;
    std::ofstream csv_;
};

} // namespace edge_ai
