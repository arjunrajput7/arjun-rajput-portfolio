// SPDX-License-Identifier: MIT
//
// Bounded, thread-safe SPSC/MPSC frame queue with a drop-oldest overflow policy.
// One producer thread (the GStreamer appsink callback) pushes frames; one or
// more consumer threads (RTSP feeder, raw memory sink) pop them. When a slow
// consumer lets the queue fill up, the oldest frame is dropped so latency stays
// bounded instead of growing without limit.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace gstpipe {

template <typename T>
class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    // Push a frame. Returns false (and drops the oldest entry) on overflow.
    bool push(T value) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (closed_) return false;
            if (q_.size() >= capacity_) {
                q_.pop_front();
                dropped = true;
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            q_.push_back(std::move(value));
            pushed_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
        return !dropped;
    }

    // Block until a frame is available or the queue is closed.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        popped_.fetch_add(1, std::memory_order_relaxed);
        return v;
    }

    // Non-blocking variant.
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        popped_.fetch_add(1, std::memory_order_relaxed);
        return v;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return q_.size();
    }

    std::uint64_t pushed()  const { return pushed_.load(std::memory_order_relaxed); }
    std::uint64_t popped()  const { return popped_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex        m_;
    std::condition_variable   cv_;
    std::deque<T>             q_;
    std::size_t               capacity_;
    bool                      closed_ = false;
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> popped_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace gstpipe
