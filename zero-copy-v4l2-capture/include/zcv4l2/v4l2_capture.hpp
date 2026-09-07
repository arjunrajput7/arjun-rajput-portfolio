// SPDX-License-Identifier: MIT
//
// Zero-copy V4L2 capture engine.
//
// Talks to a V4L2 device (/dev/videoN) using raw ioctl() calls only -- no
// libv4l, no OpenCV, no GStreamer. Buffers are negotiated with the driver via
// VIDIOC_REQBUFS and mapped into userspace with mmap() (V4L2_MEMORY_MMAP), so
// captured frames are never copied between kernel and userspace. Each mapped
// buffer can optionally be exported as a DMABUF file descriptor (VIDIOC_EXPBUF)
// for zero-copy hand-off to a GPU / encoder / another driver.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <linux/videodev2.h>
#include <sys/time.h>

namespace zcv4l2 {

enum class IoMethod {
    Mmap,   // VIDIOC_REQBUFS + mmap(); driver owns the allocation
    UserPtr // VIDIOC_REQBUFS with V4L2_MEMORY_USERPTR; app owns the allocation
};

struct Config {
    std::string device        = "/dev/video0";
    uint32_t    width         = 1280;
    uint32_t    height        = 720;
    uint32_t    pixelformat   = V4L2_PIX_FMT_YUYV; // FourCC, e.g. YUYV / MJPG / NV12
    uint32_t    buffer_count  = 4;
    IoMethod    io            = IoMethod::Mmap;
    bool        export_dmabuf = false; // VIDIOC_EXPBUF each buffer after mapping
};

// A dequeued frame. `data`/`bytes` point into a driver-owned mmap region and are
// only valid until release() is called for this frame.
struct Frame {
    const uint8_t* data      = nullptr;
    size_t         bytes     = 0;
    uint32_t       index     = 0;   // buffer index, needed by release()
    uint32_t       sequence  = 0;   // driver frame counter (gap => dropped frame)
    timeval        timestamp{};     // buffer timestamp from the driver
    int            dmabuf_fd = -1;  // >=0 when Config::export_dmabuf is set
    bool           had_error = false; // V4L2_BUF_FLAG_ERROR was set
};

struct Stats {
    uint64_t frames_captured = 0;
    uint64_t frames_dropped  = 0;  // inferred from sequence-number gaps
    uint64_t frames_errored  = 0;  // V4L2_BUF_FLAG_ERROR seen
    uint64_t bytes_captured  = 0;
    double   last_latency_ms = 0.0; // buffer timestamp -> DQBUF wallclock
    double   avg_latency_ms  = 0.0;
};

class V4L2Capture {
public:
    explicit V4L2Capture(Config cfg);
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&)            = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // Full setup: open() + QUERYCAP + S_FMT + REQBUFS + mmap + (EXPBUF) + QBUF.
    void open();

    // VIDIOC_STREAMON.
    void start();

    // poll() with `timeout_ms`, then VIDIOC_DQBUF. Returns false on timeout.
    // The caller MUST call release(frame) once done with the pixels.
    bool next(Frame& out, int timeout_ms = 2000);

    // VIDIOC_QBUF -- hand the buffer back to the driver.
    void release(const Frame& f);

    // VIDIOC_STREAMOFF (also re-queues everything so start() can be called again).
    void stop();

    // munmap + close.
    void close();

    const Stats&       stats()  const { return stats_; }
    const v4l2_format& format() const { return fmt_; }
    int                fd()     const { return fd_; }

private:
    struct BufferSlot {
        void*  start     = nullptr; // mmap address (MMAP)
        size_t length     = 0;
        int    dmabuf_fd = -1;
    };

    void xioctl(unsigned long req, void* arg, const char* what);
    void query_caps();
    void set_format();
    void request_buffers();
    void map_buffers();
    void queue_buffer(uint32_t index);
    void queue_all();
    void unmap_buffers();

    Config                   cfg_;
    int                      fd_ = -1;
    v4l2_format              fmt_{};
    std::vector<BufferSlot> slots_;
    Stats                    stats_{};
    uint32_t                 last_sequence_      = 0;
    bool                     have_last_sequence_ = false;
    double                   latency_accum_ms_   = 0.0;
    bool                     streaming_          = false;
};

// FourCC helpers for CLI parsing / logging.
uint32_t fourcc_from_string(const std::string& s);
std::string fourcc_to_string(uint32_t f);

} // namespace zcv4l2
