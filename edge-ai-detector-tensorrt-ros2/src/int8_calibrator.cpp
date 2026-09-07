// SPDX-License-Identifier: MIT
#include "edge_ai_detector/int8_calibrator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <opencv2/opencv.hpp> // build-time only (engine builder / calibration)

namespace fs = std::filesystem;

namespace edge_ai {

namespace {
void cuda_or_die(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[calib] %s: %s\n", what, cudaGetErrorString(e));
        std::abort();
    }
}
} // namespace

Int8EntropyCalibrator::Int8EntropyCalibrator(std::string image_dir, std::string cache_path,
                                             int width, int height, int batch,
                                             std::string input_tensor)
    : image_dir_(std::move(image_dir)),
      cache_path_(std::move(cache_path)),
      input_tensor_(std::move(input_tensor)),
      w_(width),
      h_(height),
      batch_(batch) {
    for (const auto& e : fs::directory_iterator(image_dir_)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
            files_.push_back(e.path().string());
    }
    std::sort(files_.begin(), files_.end());
    std::fprintf(stderr, "[calib] %zu calibration images in %s\n", files_.size(),
                 image_dir_.c_str());

    input_bytes_ = std::size_t(batch_) * 3 * h_ * w_ * sizeof(float);
    cuda_or_die(cudaMalloc(&device_input_, input_bytes_), "cudaMalloc calib input");
}

Int8EntropyCalibrator::~Int8EntropyCalibrator() {
    if (device_input_) cudaFree(device_input_);
}

bool Int8EntropyCalibrator::load_next_batch(std::vector<float>& nchw) {
    if (cursor_ >= files_.size()) return false;

    nchw.assign(std::size_t(batch_) * 3 * h_ * w_, 0.5f);
    const std::size_t plane = std::size_t(w_) * h_;

    int loaded = 0;
    for (; loaded < batch_ && cursor_ < files_.size(); ++cursor_) {
        cv::Mat img = cv::imread(files_[cursor_], cv::IMREAD_COLOR); // BGR
        if (img.empty()) continue;

        const float scale = std::min(float(w_) / img.cols, float(h_) / img.rows);
        const int nw = int(std::round(img.cols * scale));
        const int nh = int(std::round(img.rows * scale));
        cv::Mat resized;
        cv::resize(img, resized, {nw, nh});
        const int px = (w_ - nw) / 2, py = (h_ - nh) / 2;

        float* base = nchw.data() + std::size_t(loaded) * 3 * plane;
        for (int y = 0; y < nh; ++y) {
            const auto* row = resized.ptr<cv::Vec3b>(y);
            float* orow = base + std::size_t(y + py) * w_ + px;
            for (int x = 0; x < nw; ++x) {
                orow[x]             = row[x][2] / 255.f; // R
                orow[x + plane]     = row[x][1] / 255.f; // G
                orow[x + 2 * plane] = row[x][0] / 255.f; // B
            }
        }
        ++loaded;
    }
    return loaded > 0;
}

bool Int8EntropyCalibrator::getBatch(void* bindings[], const char* names[], int nb_bindings) noexcept {
    std::vector<float> host;
    if (!load_next_batch(host)) return false;

    cuda_or_die(cudaMemcpy(device_input_, host.data(), host.size() * sizeof(float),
                           cudaMemcpyHostToDevice),
                "calib H2D");

    for (int i = 0; i < nb_bindings; ++i) {
        if (input_tensor_.empty() || input_tensor_ == names[i]) bindings[i] = device_input_;
    }
    std::fprintf(stderr, "[calib] batch @ %zu / %zu\n", cursor_, files_.size());
    return true;
}

const void* Int8EntropyCalibrator::readCalibrationCache(std::size_t& length) noexcept {
    cache_.clear();
    std::ifstream f(cache_path_, std::ios::binary);
    if (!f) {
        length = 0;
        return nullptr;
    }
    cache_.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    length = cache_.size();
    std::fprintf(stderr, "[calib] loaded calibration cache (%zu bytes)\n", length);
    return cache_.data();
}

void Int8EntropyCalibrator::writeCalibrationCache(const void* ptr, std::size_t length) noexcept {
    std::ofstream f(cache_path_, std::ios::binary | std::ios::trunc);
    if (f) {
        f.write(static_cast<const char*>(ptr), std::streamsize(length));
        std::fprintf(stderr, "[calib] wrote calibration cache (%zu bytes) -> %s\n", length,
                     cache_path_.c_str());
    }
}

} // namespace edge_ai
