// SPDX-License-Identifier: MIT
//
// IInt8EntropyCalibrator2 that streams a folder of calibration images through
// the same letterbox pre-processing the runtime uses. Reads/writes a calibration
// cache so the (slow) calibration pass only runs once.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

namespace edge_ai {

class Int8EntropyCalibrator final : public nvinfer1::IInt8EntropyCalibrator2 {
public:
    Int8EntropyCalibrator(std::string image_dir, std::string cache_path, int width, int height,
                          int batch, std::string input_tensor);
    ~Int8EntropyCalibrator() override;

    int  getBatchSize() const noexcept override { return batch_; }
    bool getBatch(void* bindings[], const char* names[], int nb_bindings) noexcept override;
    const void* readCalibrationCache(std::size_t& length) noexcept override;
    void        writeCalibrationCache(const void* ptr, std::size_t length) noexcept override;

private:
    bool load_next_batch(std::vector<float>& nchw);

    std::string           image_dir_;
    std::string           cache_path_;
    std::string           input_tensor_;
    int                   w_, h_, batch_;
    std::vector<std::string> files_;
    std::size_t           cursor_ = 0;
    void*                 device_input_ = nullptr;
    std::size_t           input_bytes_  = 0;
    std::vector<char>     cache_;
};

} // namespace edge_ai
