// SPDX-License-Identifier: MIT
#include "zcv4l2/v4l2_capture.hpp"
#include "zcv4l2/logging.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zcv4l2 {

namespace {
double timeval_to_ms(const timeval& tv) {
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
double now_monotonic_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
} // namespace

uint32_t fourcc_from_string(const std::string& s) {
    char c[4] = {' ', ' ', ' ', ' '};
    for (size_t i = 0; i < s.size() && i < 4; ++i) c[i] = s[i];
    return v4l2_fourcc(c[0], c[1], c[2], c[3]);
}

std::string fourcc_to_string(uint32_t f) {
    return {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff),
            char((f >> 24) & 0xff)};
}

V4L2Capture::V4L2Capture(Config cfg) : cfg_(std::move(cfg)) {}

V4L2Capture::~V4L2Capture() {
    try {
        stop();
    } catch (...) {
    }
    close();
}

// Retry-on-EINTR ioctl wrapper. Throws SystemError on hard failure.
void V4L2Capture::xioctl(unsigned long req, void* arg, const char* what) {
    int r;
    do {
        r = ::ioctl(fd_, req, arg);
    } while (r == -1 && errno == EINTR);
    if (r == -1) throw SystemError(what, errno);
}

void V4L2Capture::open() {
    struct stat st{};
    if (::stat(cfg_.device.c_str(), &st) == -1) throw SystemError("stat(" + cfg_.device + ")", errno);
    if (!S_ISCHR(st.st_mode)) throw SystemError(cfg_.device + " is not a character device", ENODEV);

    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd_ == -1) throw SystemError("open(" + cfg_.device + ")", errno);

    query_caps();
    set_format();
    request_buffers();
    map_buffers();
    queue_all();
}

void V4L2Capture::query_caps() {
    v4l2_capability cap{};
    xioctl(VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP");

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        throw SystemError("device has no V4L2_CAP_VIDEO_CAPTURE", ENODEV);
    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        throw SystemError("device has no V4L2_CAP_STREAMING (mmap/dmabuf unsupported)", ENODEV);

    ZCV_LOG("driver=%s card=%s bus=%s", cap.driver, cap.card, cap.bus_info);
}

void V4L2Capture::set_format() {
    fmt_.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt_.fmt.pix.width       = cfg_.width;
    fmt_.fmt.pix.height      = cfg_.height;
    fmt_.fmt.pix.pixelformat = cfg_.pixelformat;
    fmt_.fmt.pix.field       = V4L2_FIELD_ANY;
    xioctl(VIDIOC_S_FMT, &fmt_, "VIDIOC_S_FMT");

    // The driver may have adjusted the request -- honour whatever it returned.
    if (fmt_.fmt.pix.pixelformat != cfg_.pixelformat) {
        ZCV_WARN("driver substituted pixelformat %s -> %s",
                 fourcc_to_string(cfg_.pixelformat).c_str(),
                 fourcc_to_string(fmt_.fmt.pix.pixelformat).c_str());
        cfg_.pixelformat = fmt_.fmt.pix.pixelformat;
    }
    if (fmt_.fmt.pix.width != cfg_.width || fmt_.fmt.pix.height != cfg_.height) {
        ZCV_WARN("driver substituted resolution %ux%u -> %ux%u", cfg_.width, cfg_.height,
                 fmt_.fmt.pix.width, fmt_.fmt.pix.height);
        cfg_.width  = fmt_.fmt.pix.width;
        cfg_.height = fmt_.fmt.pix.height;
    }
    ZCV_LOG("format %ux%u %s, %u bytes/line, %u byte image", fmt_.fmt.pix.width,
            fmt_.fmt.pix.height, fourcc_to_string(fmt_.fmt.pix.pixelformat).c_str(),
            fmt_.fmt.pix.bytesperline, fmt_.fmt.pix.sizeimage);
}

void V4L2Capture::request_buffers() {
    v4l2_requestbuffers req{};
    req.count  = cfg_.buffer_count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = (cfg_.io == IoMethod::Mmap) ? V4L2_MEMORY_MMAP : V4L2_MEMORY_USERPTR;
    xioctl(VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

    if (req.count < 2) throw SystemError("driver granted < 2 buffers, cannot stream", ENOMEM);
    if (req.count != cfg_.buffer_count)
        ZCV_WARN("requested %u buffers, driver granted %u", cfg_.buffer_count, req.count);

    cfg_.buffer_count = req.count;
    slots_.resize(req.count);
}

void V4L2Capture::map_buffers() {
    for (uint32_t i = 0; i < cfg_.buffer_count; ++i) {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        xioctl(VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

        void* p = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (p == MAP_FAILED) throw SystemError("mmap buffer " + std::to_string(i), errno);

        slots_[i].start  = p;
        slots_[i].length = buf.length;

        if (cfg_.export_dmabuf) {
            v4l2_exportbuffer exp{};
            exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index = i;
            exp.flags = O_CLOEXEC | O_RDWR;
            xioctl(VIDIOC_EXPBUF, &exp, "VIDIOC_EXPBUF");
            slots_[i].dmabuf_fd = exp.fd;
            ZCV_LOG("buffer %u exported as dmabuf fd %d (%zu bytes)", i, exp.fd, buf.length);
        }
    }
}

void V4L2Capture::queue_buffer(uint32_t index) {
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;
    xioctl(VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
}

void V4L2Capture::queue_all() {
    for (uint32_t i = 0; i < cfg_.buffer_count; ++i) queue_buffer(i);
}

void V4L2Capture::start() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
    streaming_ = true;
    have_last_sequence_ = false;
    ZCV_LOG("streaming started (%u buffers, %s)", cfg_.buffer_count,
            cfg_.io == IoMethod::Mmap ? "MMAP" : "USERPTR");
}

bool V4L2Capture::next(Frame& out, int timeout_ms) {
    pollfd pfd{fd_, POLLIN, 0};
    int pr;
    do {
        pr = ::poll(&pfd, 1, timeout_ms);
    } while (pr == -1 && errno == EINTR);

    if (pr == -1) throw SystemError("poll", errno);
    if (pr == 0) {
        ZCV_WARN("poll timed out after %d ms -- possible frame stall", timeout_ms);
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP)) throw SystemError("poll reported POLLERR/POLLHUP", EIO);

    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    int r;
    do {
        r = ::ioctl(fd_, VIDIOC_DQBUF, &buf);
    } while (r == -1 && errno == EINTR);
    if (r == -1) {
        if (errno == EAGAIN) return false;             // nothing ready yet
        throw SystemError("VIDIOC_DQBUF", errno);
    }

    const bool errored = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    if (errored) {
        ++stats_.frames_errored;
        ZCV_WARN("buffer %u flagged V4L2_BUF_FLAG_ERROR (corrupt frame)", buf.index);
    }

    // Sequence-gap => the driver dropped frame(s) we never saw.
    if (have_last_sequence_) {
        uint32_t expected = last_sequence_ + 1;
        if (buf.sequence > expected) {
            uint32_t missed = buf.sequence - expected;
            stats_.frames_dropped += missed;
            ZCV_WARN("dropped %u frame(s): seq jumped %u -> %u", missed, last_sequence_,
                     buf.sequence);
        }
    }
    last_sequence_      = buf.sequence;
    have_last_sequence_ = true;

    const double latency = now_monotonic_ms() - timeval_to_ms(buf.timestamp);
    stats_.last_latency_ms = latency;
    latency_accum_ms_ += latency;
    ++stats_.frames_captured;
    stats_.bytes_captured += buf.bytesused;
    stats_.avg_latency_ms = latency_accum_ms_ / double(stats_.frames_captured);

    out.data      = static_cast<const uint8_t*>(slots_[buf.index].start);
    out.bytes     = buf.bytesused;
    out.index     = buf.index;
    out.sequence  = buf.sequence;
    out.timestamp = buf.timestamp;
    out.dmabuf_fd = slots_[buf.index].dmabuf_fd;
    out.had_error = errored;
    return true;
}

void V4L2Capture::release(const Frame& f) {
    queue_buffer(f.index);
}

void V4L2Capture::stop() {
    if (!streaming_ || fd_ == -1) return;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    try {
        xioctl(VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
    } catch (const SystemError& e) {
        ZCV_WARN("STREAMOFF failed: %s", e.what());
    }
    streaming_ = false;
    ZCV_LOG("streaming stopped: captured=%llu dropped=%llu errored=%llu avg_latency=%.2fms",
            (unsigned long long)stats_.frames_captured, (unsigned long long)stats_.frames_dropped,
            (unsigned long long)stats_.frames_errored, stats_.avg_latency_ms);
}

void V4L2Capture::unmap_buffers() {
    for (auto& s : slots_) {
        if (s.dmabuf_fd >= 0) {
            ::close(s.dmabuf_fd);
            s.dmabuf_fd = -1;
        }
        if (s.start && s.length) {
            ::munmap(s.start, s.length);
            s.start  = nullptr;
            s.length = 0;
        }
    }
    slots_.clear();
}

void V4L2Capture::close() {
    unmap_buffers();
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace zcv4l2
