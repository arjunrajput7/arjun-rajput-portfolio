// SPDX-License-Identifier: MIT
//
// Thin RAII wrapper around a serialized TensorRT engine (TensorRT 8.5 - 10.x).
// Loads a *.engine file, binds every I/O tensor to a device buffer, and runs
// inference with enqueueV3() on a dedicated CUDA stream.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

namespace edge_ai {

// Forwards std::unique_ptr deleters for TensorRT interface objects.
struct TrtDeleter {
    template <typename T>
    void operator()(T* p) const noexcept {
        delete p; // TensorRT 8.5+: interface objects are deleted with `delete`
    }
};
template <typename T>
using TrtUnique = std::unique_ptr<T, TrtDeleter>;

class TrtLogger : public nvinfer1::ILogger {
public:
    explicit TrtLogger(Severity min = Severity::kWARNING) : min_(min) {}
    void log(Severity s, const char* msg) noexcept override;

private:
    Severity min_;
};

struct TensorBinding {
    std::string           name;
    bool                  is_input = false;
    nvinfer1::Dims        dims{};
    nvinfer1::DataType    dtype = nvinfer1::DataType::kFLOAT;
    std::size_t           bytes = 0;
    void*                 device = nullptr;   // cudaMalloc'd
    std::vector<float>    host;               // staging buffer (float view)
};

struct InferConfig {
    std::string engine_path;
    int         device      = 0;   // CUDA device ordinal
    int         input_w     = 640;
    int         input_h     = 640;
    std::string input_name;         // empty => first input tensor
};

class TrtEngine {
public:
    explicit TrtEngine(InferConfig cfg);
    ~TrtEngine();

    TrtEngine(const TrtEngine&)            = delete;
    TrtEngine& operator=(const TrtEngine&) = delete;

    void load();

    // Copy a prepared NCHW float32 blob (size = 3*H*W) to the input device
    // buffer, run the network, and copy every output back to host. Returns the
    // wall-clock GPU time in milliseconds (CUDA events).
    float infer(const float* nchw_input, std::size_t input_elems);

    const TensorBinding& input()  const { return bindings_[input_idx_]; }
    const std::vector<TensorBinding>& outputs() const { return outputs_; }
    const std::vector<float>& output_host(std::size_t i) const { return outputs_[i].host; }

    int input_w() const { return cfg_.input_w; }
    int input_h() const { return cfg_.input_h; }

private:
    static std::size_t dtype_size(nvinfer1::DataType d);
    static std::size_t volume(const nvinfer1::Dims& d);
    void   allocate_bindings();

    InferConfig                 cfg_;
    TrtLogger                   logger_;
    TrtUnique<nvinfer1::IRuntime>          runtime_;
    TrtUnique<nvinfer1::ICudaEngine>       engine_;
    TrtUnique<nvinfer1::IExecutionContext> context_;

    std::vector<TensorBinding>  bindings_;
    std::vector<TensorBinding>  outputs_;   // views copied out for convenience
    int                         input_idx_ = -1;

    cudaStream_t stream_ = nullptr;
    cudaEvent_t  ev_start_ = nullptr;
    cudaEvent_t  ev_stop_  = nullptr;
};

} // namespace edge_ai
