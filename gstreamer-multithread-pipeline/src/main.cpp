// SPDX-License-Identifier: MIT
//
// gstpipe -- multi-threaded GStreamer capture pipeline with RTSP restream and a
// secondary in-memory raw sink, all constructed via the GStreamer C API.
//
//   gstpipe [--device /dev/video0] [--width 1280] [--height 720] [--fps 30]
//           [--bitrate 4000] [--encoder auto|v4l2|x264]
//           [--rtsp-port 8554] [--mount /live]
//
// Threads at runtime:
//   * GLib main loop      -- bus messages + RTSP server I/O
//   * enc  streaming thr  -- tee -> encoder -> appsink  (GStreamer-owned)
//   * raw  streaming thr  -- tee -> convert  -> appsink  (GStreamer-owned)
//   * RTSP feeder thread  -- drains the encoded FrameQueue into appsrc
//   * raw consumer thread -- drains the raw FrameQueue (checksum / stats here)
//   * reporter thread     -- prints latency + queue telemetry once a second
#include "gstpipe/pipeline.hpp"
#include "gstpipe/rtsp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <glib-unix.h>
#include <gst/gst.h>

namespace {

GMainLoop* g_loop = nullptr;

gboolean on_unix_signal(gpointer) {
    g_printerr("\n[gstpipe] signal received, shutting down\n");
    if (g_loop) g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

const char* opt(int argc, char** argv, const char* k, const char* dflt) {
    for (int i = 1; i < argc - 1; ++i)
        if (!std::strcmp(argv[i], k)) return argv[i + 1];
    return dflt;
}

gstpipe::Encoder parse_encoder(const char* s) {
    if (!std::strcmp(s, "v4l2")) return gstpipe::Encoder::V4L2H264;
    if (!std::strcmp(s, "x264")) return gstpipe::Encoder::X264;
    return gstpipe::Encoder::Auto;
}

} // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    gstpipe::PipelineConfig pcfg;
    pcfg.device       = opt(argc, argv, "--device", "/dev/video0");
    pcfg.width        = std::atoi(opt(argc, argv, "--width", "1280"));
    pcfg.height       = std::atoi(opt(argc, argv, "--height", "720"));
    pcfg.fps          = std::atoi(opt(argc, argv, "--fps", "30"));
    pcfg.bitrate_kbps = std::atoi(opt(argc, argv, "--bitrate", "4000"));
    pcfg.encoder      = parse_encoder(opt(argc, argv, "--encoder", "auto"));

    gstpipe::RtspConfig rcfg;
    rcfg.service = opt(argc, argv, "--rtsp-port", "8554");
    rcfg.mount   = opt(argc, argv, "--mount", "/live");
    rcfg.fps     = pcfg.fps;

    g_loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, on_unix_signal, nullptr);
    g_unix_signal_add(SIGTERM, on_unix_signal, nullptr);

    std::atomic<bool> run{true};

    try {
        gstpipe::Pipeline pipeline(pcfg);
        pipeline.set_error_handler([&](const std::string&) {
            run.store(false);
            if (g_loop) g_main_loop_quit(g_loop);
        });
        pipeline.build();

        gstpipe::RtspServer rtsp(rcfg, pipeline.encoded_queue());
        rtsp.attach(nullptr);

        pipeline.start();
        g_printerr("[gstpipe] encoder caps: %s\n", pipeline.encoder_caps().c_str());
        g_printerr("[gstpipe] RTSP stream: %s\n", rtsp.url().c_str());

        // ---- secondary raw consumer -------------------------------------
        std::thread raw_consumer([&] {
            auto& q = pipeline.raw_queue();
            std::uint64_t frames = 0, bytes = 0;
            while (run.load()) {
                auto f = q.pop();
                if (!f) break;
                // Real work would go here (SHM publish, CV, recording...).
                // We just fold the data so the compiler can't elide the copy.
                std::uint32_t acc = 0;
                for (std::size_t i = 0; i < f->data.size(); i += 4096) acc += f->data[i];
                bytes += f->data.size();
                if ((++frames % 120) == 0)
                    g_printerr("[raw-sink] %llu frames  %llu MiB  %dx%d %s  (acc=%u)\n",
                               (unsigned long long)frames,
                               (unsigned long long)(bytes >> 20), f->width, f->height,
                               f->format.c_str(), acc);
            }
        });

        // ---- 1 Hz telemetry -------------------------------------------
        std::thread reporter([&] {
            auto& eq = pipeline.encoded_queue();
            auto& rq = pipeline.raw_queue();
            while (run.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto s = pipeline.appsink_latency().snapshot();
                g_printerr(
                    "[stat] enc_q(size=%zu push=%llu pop=%llu drop=%llu) "
                    "raw_q(size=%zu drop=%llu) rtsp_served=%llu "
                    "lat_ms(mean=%.1f p95=%.1f max=%.1f n=%llu)\n",
                    eq.size(), (unsigned long long)eq.pushed(), (unsigned long long)eq.popped(),
                    (unsigned long long)eq.dropped(), rq.size(), (unsigned long long)rq.dropped(),
                    (unsigned long long)rtsp.frames_served(), s.mean_ms, s.p95_ms, s.max_ms,
                    (unsigned long long)s.count);
            }
        });

        g_main_loop_run(g_loop);

        // ---- teardown ------------------------------------------------
        run.store(false);
        pipeline.stop();     // closes both queues -> unblocks consumers
        rtsp.shutdown();
        if (raw_consumer.joinable()) raw_consumer.join();
        if (reporter.joinable()) reporter.join();
    } catch (const std::exception& e) {
        g_printerr("[gstpipe] fatal: %s\n", e.what());
        if (g_loop) g_main_loop_unref(g_loop);
        return 1;
    }

    g_main_loop_unref(g_loop);
    g_printerr("[gstpipe] clean exit\n");
    return 0;
}
