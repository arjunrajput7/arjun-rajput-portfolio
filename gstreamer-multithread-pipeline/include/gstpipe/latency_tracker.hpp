// SPDX-License-Identifier: MIT
//
// Rolling latency statistics (min / max / mean / p95) over a fixed-size window.
// Thread-safe: the appsink producer records samples, a reporter thread reads the
// snapshot.
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gstpipe {

struct LatencySnapshot {
    std::uint64_t count = 0;
    double min_ms  = 0.0;
    double max_ms  = 0.0;
    double mean_ms = 0.0;
    double p95_ms  = 0.0;
};

class LatencyTracker {
public:
    explicit LatencyTracker(std::size_t window = 512) : window_(window) {
        samples_.reserve(window);
    }

    void add(double ms) {
        std::lock_guard<std::mutex> lk(m_);
        if (samples_.size() < window_) {
            samples_.push_back(ms);
        } else {
            samples_[cursor_] = ms;
        }
        cursor_ = (cursor_ + 1) % window_;
        ++total_;
        sum_ += ms;
    }

    LatencySnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        LatencySnapshot s;
        s.count = total_;
        if (samples_.empty()) return s;

        std::vector<double> sorted(samples_);
        std::sort(sorted.begin(), sorted.end());
        s.min_ms  = sorted.front();
        s.max_ms  = sorted.back();
        s.mean_ms = sum_ / double(total_);
        const std::size_t idx =
            std::min(sorted.size() - 1, std::size_t(sorted.size() * 0.95));
        s.p95_ms = sorted[idx];
        return s;
    }

private:
    mutable std::mutex   m_;
    std::vector<double>  samples_;
    std::size_t          window_;
    std::size_t          cursor_ = 0;
    std::uint64_t        total_  = 0;
    double               sum_    = 0.0;
};

} // namespace gstpipe
