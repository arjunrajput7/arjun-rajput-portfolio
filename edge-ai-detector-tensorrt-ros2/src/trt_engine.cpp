// SPDX-License-Identifier: MIT
#include "edge_ai_detector/trt_engine.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace edge_ai {

namespace {
void cuda_check(cudaError_t e, const char* what) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(e));
}
} // namespace

void TrtLogger::log(Severity s, const char* msg) noexcept {
    if (s > min_) return;
    const char* tag = "INFO";
    switch (s) {
        case Severity::kINTERNAL_ERROR: tag = "TRT-FATAL"; break;
        case Severity::kERROR:          tag = "TRT-ERROR"; break;
        case Severity::kWARNING:        tag = "TRT-WARN";  break;
        case Severity::kINFO:           tag = "TRT-INFO";  break;
        default:                        tag = "TRT-DBG";   break;
    }
    std::fprintf(stderr, "[%s] %s\n", tag, msg);
}

TrtEngine::TrtEngine(InferConfig cfg) : cfg_(std::move(cfg)) {}

TrtEngine::~TrtEngine() {
    for (auto& b : bindings_)
        if (b.device) cudaFree(b.device);
    if (ev_start_) cudaEventDestroy(ev_start_);
    if (ev_stop_)  cudaEventDestroy(ev_stop_);
    if (stream_)   cudaStreamDestroy(stream_);
}

std::size_t TrtEngine::dtype_size(nvinfer1::DataType d) {
    switch (d) {
        case nvinfer1::DataType::kFLOAT: return 4;
        case nvinfer1::DataType::kHALF:  return 2;
        case nvinfer1::DataType::kINT32: return 4;
        case nvinfer1::DataType::kINT8:  return 1;
        case nvinfer1::DataType::kBOOL:  return 1;
        default:                         return 4;
    }
}

std::size_t TrtEngine::volume(const nvinfer1::Dims& d) {
    std::size_t v = 1;
    for (int i = 0; i < d.nbDims; ++i) v *= std::size_t(d.d[i] < 0 ? 1 : d.d[i]);
    return v;
}

void TrtEngine::load() {
    cuda_check(cudaSetDevice(cfg_.device), "cudaSetDevice");

    std::ifstream f(cfg_.engine_path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open engine: " + cfg_.engine_path);
    const std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<char> blob(std::size_t(n));
    if (!f.read(blob.data(), n)) throw std::runtime_error("short read on engine file");

    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_) throw std::runtime_error("createInferRuntime failed");

    engine_.reset(runtime_->deserializeCudaEngine(blob.data(), blob.size()));
    if (!engine_) throw std::runtime_error("deserializeCudaEngine failed (version / arch mismatch?)");

    context_.reset(engine_->createExecutionContext());
    if (!context_) throw std::runtime_error("createExecutionContext failed");

    cuda_check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
    cuda_check(cudaEventCreate(&ev_start_), "cudaEventCreate");
    cuda_check(cudaEventCreate(&ev_stop_),  "cudaEventCreate");

    allocate_bindings();
}

void TrtEngine::allocate_bindings() {
    const int nb = engine_->getNbIOTensors();
    bindings_.reserve(nb);

    for (int i = 0; i < nb; ++i) {
        const char* name = engine_->getIOTensorName(i);
        TensorBinding b;
        b.name     = name;
        b.is_input = engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT;
        b.dtype    = engine_->getTensorDataType(name);
        b.dims     = engine_->getTensorShape(name);

        // Resolve dynamic dims for the input to the configured letterbox size.
        if (b.is_input) {
            nvinfer1::Dims d = b.dims;
            if (d.nbDims == 4) {
                if (d.d[0] < 0) d.d[0] = 1;
                if (d.d[2] < 0) d.d[2] = cfg_.input_h;
                if (d.d[3] < 0) d.d[3] = cfg_.input_w;
            }
            if (!context_->setInputShape(name, d))
                throw std::runtime_error(std::string("setInputShape failed for ") + name);
            b.dims = d;
        }

        b.bytes = volume(b.dims) * dtype_size(b.dtype);
        cuda_check(cudaMalloc(&b.device, b.bytes), "cudaMalloc binding");
        if (!context_->setTensorAddress(name, b.device))
            throw std::runtime_error(std::string("setTensorAddress failed for ") + name);

        b.host.resize(b.bytes / sizeof(float) + 1);
        bindings_.push_back(std::move(b));

        std::fprintf(stderr, "[trt] %-16s %-6s dims=[", name, bindings_.back().is_input ? "input" : "output");
        for (int k = 0; k < bindings_.back().dims.nbDims; ++k)
            std::fprintf(stderr, "%s%d", k ? "," : "", int(bindings_.back().dims.d[k]));
        std::fprintf(stderr, "] %zu bytes\n", bindings_.back().bytes);
    }

    for (int i = 0; i < int(bindings_.size()); ++i) {
        if (bindings_[i].is_input) {
            if (cfg_.input_name.empty() || cfg_.input_name == bindings_[i].name) {
                if (input_idx_ < 0) input_idx_ = i;
            }
        } else {
            outputs_.push_back(bindings_[i]); // shallow copy: device ptr shared, host resized below
        }
    }
    if (input_idx_ < 0) throw std::runtime_error("engine has no input tensor");
    for (auto& o : outputs_) o.host.assign(o.bytes / sizeof(float) + 1, 0.f);
}

float TrtEngine::infer(const float* nchw_input, std::size_t input_elems) {
    TensorBinding& in = bindings_[input_idx_];
    const std::size_t need = in.bytes / sizeof(float);
    if (input_elems < need)
        throw std::runtime_error("infer(): input blob smaller than engine input");

    cuda_check(cudaMemcpyAsync(in.device, nchw_input, in.bytes, cudaMemcpyHostToDevice, stream_),
               "H2D input");

    cuda_check(cudaEventRecord(ev_start_, stream_), "event start");
    if (!context_->enqueueV3(stream_)) throw std::runtime_error("enqueueV3 failed");
    cuda_check(cudaEventRecord(ev_stop_, stream_), "event stop");

    // Pull every output back.
    std::size_t oi = 0;
    for (auto& b : bindings_) {
        if (b.is_input) continue;
        TensorBinding& dst = outputs_[oi++];
        cuda_check(cudaMemcpyAsync(dst.host.data(), b.device, b.bytes, cudaMemcpyDeviceToHost,
                                   stream_),
                   "D2H output");
    }

    cuda_check(cudaStreamSynchronize(stream_), "stream sync");
    float ms = 0.f;
    cuda_check(cudaEventElapsedTime(&ms, ev_start_, ev_stop_), "event elapsed");
    return ms;
}

} // namespace edge_ai
