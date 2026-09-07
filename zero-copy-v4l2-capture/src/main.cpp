// SPDX-License-Identifier: MIT
//
// zcv4l2 -- standalone zero-copy V4L2 capture demo.
//
//   zcv4l2 [--device /dev/video0] [--width 1280] [--height 720]
//          [--format YUYV] [--buffers 4] [--frames 300]
//          [--dmabuf] [--raw-out capture.raw]
//
// Streams N frames from a V4L2 device using mmap'd driver buffers (no copies),
// reports per-frame latency / drop / error statistics, and can optionally dump
// raw frames to a file or export each buffer as a DMABUF fd.
#include "zcv4l2/v4l2_capture.hpp"
#include "zcv4l2/logging.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

const char* arg_value(int argc, char** argv, const char* key, const char* fallback) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return fallback;
}
bool arg_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (arg_flag(argc, argv, "--help") || arg_flag(argc, argv, "-h")) {
        std::puts(
            "usage: zcv4l2 [--device DEV] [--width W] [--height H] [--format FOURCC]\n"
            "              [--buffers N] [--frames N] [--dmabuf] [--raw-out FILE]");
        return 0;
    }

    zcv4l2::Config cfg;
    cfg.device        = arg_value(argc, argv, "--device", "/dev/video0");
    cfg.width         = std::strtoul(arg_value(argc, argv, "--width", "1280"), nullptr, 10);
    cfg.height        = std::strtoul(arg_value(argc, argv, "--height", "720"), nullptr, 10);
    cfg.pixelformat   = zcv4l2::fourcc_from_string(arg_value(argc, argv, "--format", "YUYV"));
    cfg.buffer_count  = std::strtoul(arg_value(argc, argv, "--buffers", "4"), nullptr, 10);
    cfg.export_dmabuf = arg_flag(argc, argv, "--dmabuf");

    const long frames_to_grab = std::strtol(arg_value(argc, argv, "--frames", "300"), nullptr, 10);
    const char* raw_out       = arg_value(argc, argv, "--raw-out", nullptr);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    int out_fd = -1;
    if (raw_out) {
        out_fd = ::open(raw_out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out_fd == -1) {
            std::fprintf(stderr, "cannot open %s: %s\n", raw_out, std::strerror(errno));
            return 1;
        }
    }

    try {
        zcv4l2::V4L2Capture cap(cfg);
        cap.open();
        cap.start();

        const double t0 = now_ms();
        long grabbed = 0;

        while (!g_stop.load() && (frames_to_grab <= 0 || grabbed < frames_to_grab)) {
            zcv4l2::Frame f;
            if (!cap.next(f, 2000)) continue; // timeout / EAGAIN

            if (!f.had_error && out_fd != -1) {
                ssize_t w = ::write(out_fd, f.data, f.bytes);
                if (w != ssize_t(f.bytes))
                    ZCV_WARN("short write to raw-out (%zd/%zu)", w, f.bytes);
            }

            if ((grabbed % 30) == 0) {
                const auto& s = cap.stats();
                ZCV_LOG("frame %ld  seq=%u  latency=%.2fms  avg=%.2fms  dropped=%llu  dmabuf_fd=%d",
                        grabbed, f.sequence, s.last_latency_ms, s.avg_latency_ms,
                        (unsigned long long)s.frames_dropped, f.dmabuf_fd);
            }

            cap.release(f); // hand the buffer straight back -- zero-copy loop
            ++grabbed;
        }

        const double elapsed_s = (now_ms() - t0) / 1000.0;
        cap.stop();

        const auto& s = cap.stats();
        std::printf("\n==== capture summary ====\n");
        std::printf("device            : %s\n", cfg.device.c_str());
        std::printf("negotiated format : %ux%u %s\n", cap.format().fmt.pix.width,
                    cap.format().fmt.pix.height,
                    zcv4l2::fourcc_to_string(cap.format().fmt.pix.pixelformat).c_str());
        std::printf("frames captured   : %llu\n", (unsigned long long)s.frames_captured);
        std::printf("frames dropped    : %llu\n", (unsigned long long)s.frames_dropped);
        std::printf("frames errored    : %llu\n", (unsigned long long)s.frames_errored);
        std::printf("data captured     : %.1f MiB\n", s.bytes_captured / (1024.0 * 1024.0));
        std::printf("wall time         : %.2f s\n", elapsed_s);
        std::printf("effective FPS     : %.1f\n", elapsed_s > 0 ? s.frames_captured / elapsed_s : 0.0);
        std::printf("avg DQBUF latency : %.2f ms\n", s.avg_latency_ms);

        if (out_fd != -1) ::close(out_fd);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        if (out_fd != -1) ::close(out_fd);
        return 1;
    }
}
