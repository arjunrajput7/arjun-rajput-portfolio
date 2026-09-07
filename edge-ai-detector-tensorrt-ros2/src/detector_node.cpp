// SPDX-License-Identifier: MIT
//
// edge_ai_detector -- a ROS 2 node that:
//   1. pulls decoded frame pointers from a GStreamer pipeline (edge_ai::GstSource),
//   2. letterboxes + packs them into an NCHW float blob,
//   3. runs a TensorRT INT8 engine (edge_ai::TrtEngine),
//   4. decodes YOLO output + NMS,
//   5. publishes vision_msgs/Detection2DArray (and optional SocketCAN telemetry),
//   6. records per-stage latency / memory to a CSV benchmark trace.
//
// The capture + inference loop runs on its own std::thread so the ROS executor
// stays free for parameter / lifecycle callbacks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/header.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>

#include <gst/gst.h>

#include "edge_ai_detector/benchmark.hpp"
#include "edge_ai_detector/gst_source.hpp"
#include "edge_ai_detector/socketcan_publisher.hpp"
#include "edge_ai_detector/trt_engine.hpp"
#include "edge_ai_detector/yolo_postprocess.hpp"

namespace {

// Letterbox a BGR8 frame into an RGB NCHW float blob normalised to [0,1].
// Returns the transform needed to map detections back to source pixels.
edge_ai::LetterboxInfo letterbox_bgr_to_nchw(const uint8_t* bgr, int src_w, int src_h, int stride,
                                             int dst_w, int dst_h, std::vector<float>& out) {
    edge_ai::LetterboxInfo lb;
    lb.src_w = src_w;
    lb.src_h = src_h;
    lb.scale = std::min(float(dst_w) / src_w, float(dst_h) / src_h);
    const int new_w = int(std::round(src_w * lb.scale));
    const int new_h = int(std::round(src_h * lb.scale));
    lb.pad_x = (dst_w - new_w) / 2;
    lb.pad_y = (dst_h - new_h) / 2;

    out.assign(std::size_t(3) * dst_w * dst_h, 0.5f); // grey pad, pre-normalised
    const std::size_t plane = std::size_t(dst_w) * dst_h;

    for (int y = 0; y < new_h; ++y) {
        const float sy = (y + 0.5f) / lb.scale - 0.5f;
        const int   iy = std::min(src_h - 1, std::max(0, int(sy)));
        const uint8_t* row = bgr + std::size_t(iy) * stride;
        float* orow = out.data() + std::size_t(y + lb.pad_y) * dst_w + lb.pad_x;
        for (int x = 0; x < new_w; ++x) {
            const float sx = (x + 0.5f) / lb.scale - 0.5f;
            const int   ix = std::min(src_w - 1, std::max(0, int(sx)));
            const uint8_t* px = row + std::size_t(ix) * 3;
            const float b = px[0] / 255.f, g = px[1] / 255.f, r = px[2] / 255.f;
            orow[x]             = r; // R plane
            orow[x + plane]     = g; // G plane
            orow[x + 2 * plane] = b; // B plane
        }
    }
    return lb;
}

} // namespace

class DetectorNode : public rclcpp::Node {
public:
    DetectorNode() : rclcpp::Node("edge_ai_detector") {
        // ---- parameters -------------------------------------------------
        engine_path_ = declare_parameter<std::string>("engine_path", "model_int8.engine");
        device_      = declare_parameter<std::string>("camera_device", "/dev/video0");
        cap_w_       = declare_parameter<int>("capture_width", 1280);
        cap_h_       = declare_parameter<int>("capture_height", 720);
        cap_fps_     = declare_parameter<int>("capture_fps", 30);
        mjpeg_       = declare_parameter<bool>("camera_mjpeg", true);
        jetson_csi_  = declare_parameter<bool>("jetson_csi", false);
        input_w_     = declare_parameter<int>("input_width", 640);
        input_h_     = declare_parameter<int>("input_height", 640);
        num_classes_ = declare_parameter<int>("num_classes", 80);
        conf_thr_    = declare_parameter<double>("conf_threshold", 0.25);
        iou_thr_     = declare_parameter<double>("iou_threshold", 0.45);
        transposed_  = declare_parameter<bool>("output_transposed", true); // YOLOv8 default
        publish_can_ = declare_parameter<bool>("publish_socketcan", false);
        can_iface_   = declare_parameter<std::string>("socketcan_iface", "can0");
        bench_csv_   = declare_parameter<std::string>("benchmark_csv", "");
        frame_id_    = declare_parameter<std::string>("frame_id", "camera_optical");

        pub_ = create_publisher<vision_msgs::msg::Detection2DArray>("~/detections", 10);

        if (!bench_csv_.empty()) bench_.open(bench_csv_);

        report_timer_ = create_wall_timer(std::chrono::seconds(2), [this] {
            RCLCPP_INFO(get_logger(), "%s", bench_.summary().c_str());
        });

        worker_ = std::thread(&DetectorNode::run, this);
    }

    ~DetectorNode() override {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

private:
    void run() {
        try {
            edge_ai::GstSourceConfig scfg;
            scfg.device     = device_;
            scfg.width      = cap_w_;
            scfg.height     = cap_h_;
            scfg.fps        = cap_fps_;
            scfg.mjpeg      = mjpeg_;
            scfg.jetson_csi = jetson_csi_;
            scfg.format     = "BGR";
            edge_ai::GstSource source(scfg);

            edge_ai::InferConfig icfg;
            icfg.engine_path = engine_path_;
            icfg.input_w     = input_w_;
            icfg.input_h     = input_h_;
            edge_ai::TrtEngine engine(icfg);
            engine.load();

            std::unique_ptr<edge_ai::SocketCanPublisher> can;
            if (publish_can_) {
                can = std::make_unique<edge_ai::SocketCanPublisher>(can_iface_);
                if (!can->open()) {
                    RCLCPP_WARN(get_logger(), "SocketCAN disabled: cannot open %s",
                                can_iface_.c_str());
                    can.reset();
                }
            }

            edge_ai::PostProcessConfig pp;
            pp.num_classes    = num_classes_;
            pp.conf_threshold = float(conf_thr_);
            pp.iou_threshold  = float(iou_thr_);
            pp.transposed     = transposed_;

            source.start();
            RCLCPP_INFO(get_logger(), "capture + inference loop running (engine=%s, input=%dx%d)",
                        engine_path_.c_str(), input_w_, input_h_);

            std::vector<float> blob;
            std::uint64_t fid = 0;

            while (!stop_.load() && rclcpp::ok()) {
                edge_ai::FrameMetrics m;
                m.frame_id = fid++;
                const auto t_loop0 = std::chrono::steady_clock::now();

                edge_ai::Frame frame = source.next(500);
                if (!frame.valid()) continue;

                // capture latency: sensor timestamp -> now
                m.capture_latency_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count() -
                    double(frame.capture_mono_ns()) / 1e6;

                edge_ai::LetterboxInfo lb;
                {
                    edge_ai::ScopedTimer t(m.preprocess_ms);
                    lb = letterbox_bgr_to_nchw(frame.data(), frame.width(), frame.height(),
                                               frame.stride(), input_w_, input_h_, blob);
                }

                m.inference_ms = engine.infer(blob.data(), blob.size());

                std::vector<edge_ai::Detection> dets;
                {
                    edge_ai::ScopedTimer t(m.postprocess_ms);
                    const auto& out = engine.outputs();
                    if (!out.empty()) {
                        // Derive rows/cols from the output tensor shape.
                        const auto& d = out[0].dims;
                        int a = d.nbDims >= 2 ? int(d.d[d.nbDims - 2]) : 0;
                        int b = d.nbDims >= 1 ? int(d.d[d.nbDims - 1]) : 0;
                        dets = edge_ai::decode_yolo(engine.output_host(0).data(), a, b, lb, pp);
                    }
                }
                m.detections = int(dets.size());

                {
                    edge_ai::ScopedTimer t(m.publish_ms);
                    publish(dets, frame.width(), frame.height());
                    if (can) can->publish(dets, frame.width(), frame.height());
                }

                m.rss_mib = edge_ai::rss_mib();
                m.end_to_end_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t_loop0)
                        .count();
                bench_.record(m);
            }

            source.stop();
        } catch (const std::exception& e) {
            RCLCPP_FATAL(get_logger(), "worker aborted: %s", e.what());
            rclcpp::shutdown();
        }
    }

    void publish(const std::vector<edge_ai::Detection>& dets, int, int) {
        vision_msgs::msg::Detection2DArray msg;
        msg.header.stamp    = now();
        msg.header.frame_id = frame_id_;
        msg.detections.reserve(dets.size());

        for (const auto& d : dets) {
            vision_msgs::msg::Detection2D det;
            det.header = msg.header;
#ifdef EDGE_AI_VISION_MSGS_HAS_POSE_CENTER
            det.bbox.center.position.x = d.x + d.w * 0.5;
            det.bbox.center.position.y = d.y + d.h * 0.5;
#else
            det.bbox.center.x = d.x + d.w * 0.5;
            det.bbox.center.y = d.y + d.h * 0.5;
#endif
            det.bbox.size_x = d.w;
            det.bbox.size_y = d.h;

            vision_msgs::msg::ObjectHypothesisWithPose hyp;
#ifdef EDGE_AI_VISION_MSGS_HAS_HYPOTHESIS
            hyp.hypothesis.class_id = std::to_string(d.class_id);
            hyp.hypothesis.score    = d.score;
#else
            hyp.id    = std::to_string(d.class_id); // Foxy/Galactic: id is a string
            hyp.score = d.score;
#endif
            det.results.push_back(hyp);
            msg.detections.push_back(std::move(det));
        }
        pub_->publish(msg);
    }

    // params
    std::string engine_path_, device_, can_iface_, bench_csv_, frame_id_;
    int cap_w_, cap_h_, cap_fps_, input_w_, input_h_, num_classes_;
    double conf_thr_, iou_thr_;
    bool mjpeg_, jetson_csi_, transposed_, publish_can_;

    rclcpp::Publisher<vision_msgs::msg::Detection2DArray>::SharedPtr pub_;
    rclcpp::TimerBase::SharedPtr report_timer_;
    edge_ai::BenchmarkTrace bench_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<DetectorNode>());
    rclcpp::shutdown();
    return 0;
}
