// SPDX-License-Identifier: MIT
//
// build_engine -- convert an ONNX detector into an optimised TensorRT engine.
//
//   build_engine --onnx yolov8n.onnx --out model_int8.engine \
//                --int8 --calib-dir ./calib_images --calib-cache calib.table \
//                --width 640 --height 640 [--fp16] [--workspace 2048]
//
// INT8 requires a calibration image folder the first time (a few hundred images
// representative of the deployment scene); afterwards the calibration table is
// reused.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <NvInfer.h>
#include <NvOnnxParser.h>

#include "edge_ai_detector/int8_calibrator.hpp"
#include "edge_ai_detector/trt_engine.hpp" // TrtLogger, TrtDeleter

using edge_ai::TrtLogger;
using edge_ai::TrtUnique;

namespace {
const char* opt(int c, char** v, const char* k, const char* d) {
    for (int i = 1; i < c - 1; ++i)
        if (!std::strcmp(v[i], k)) return v[i + 1];
    return d;
}
bool flag(int c, char** v, const char* k) {
    for (int i = 1; i < c; ++i)
        if (!std::strcmp(v[i], k)) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    const std::string onnx   = opt(argc, argv, "--onnx", "");
    const std::string out    = opt(argc, argv, "--out", "model.engine");
    const std::string cdir   = opt(argc, argv, "--calib-dir", "");
    const std::string ccache = opt(argc, argv, "--calib-cache", "calib.table");
    const int  W          = std::atoi(opt(argc, argv, "--width", "640"));
    const int  H          = std::atoi(opt(argc, argv, "--height", "640"));
    const int  batch      = std::atoi(opt(argc, argv, "--calib-batch", "8"));
    const long workspace  = std::atol(opt(argc, argv, "--workspace", "2048")); // MiB
    const bool use_int8   = flag(argc, argv, "--int8");
    const bool use_fp16   = flag(argc, argv, "--fp16");

    if (onnx.empty()) {
        std::fprintf(stderr, "usage: build_engine --onnx M.onnx --out M.engine [--int8 --calib-dir D]\n");
        return 2;
    }

    TrtLogger logger(nvinfer1::ILogger::Severity::kINFO);

    TrtUnique<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));

    // TensorRT < 10 needs the explicit-batch flag; TensorRT 10 removed it
    // (explicit batch is the only mode) so createNetworkV2(0) is correct there.
#if defined(NV_TENSORRT_MAJOR) && NV_TENSORRT_MAJOR < 10
    const uint32_t net_flags =
        1U << int(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
#else
    const uint32_t net_flags = 0;
#endif
    TrtUnique<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(net_flags));
    TrtUnique<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger));

    if (!parser->parseFromFile(onnx.c_str(),
                               int(nvinfer1::ILogger::Severity::kWARNING))) {
        std::fprintf(stderr, "[build] failed to parse %s\n", onnx.c_str());
        for (int i = 0; i < parser->getNbErrors(); ++i)
            std::fprintf(stderr, "  %s\n", parser->getError(i)->desc());
        return 1;
    }

    TrtUnique<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
    config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                               std::size_t(workspace) * 1024 * 1024);

    // Add an optimisation profile only if the ONNX input actually has a dynamic
    // dimension -- a fully static network rejects setDimensions().
    nvinfer1::ITensor* in = network->getInput(0);
    const char* in_name   = in->getName();
    const nvinfer1::Dims in_dims = in->getDimensions();
    bool dynamic = false;
    for (int i = 0; i < in_dims.nbDims; ++i)
        if (in_dims.d[i] < 0) dynamic = true;

    if (dynamic) {
        nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
        for (auto sel : {nvinfer1::OptProfileSelector::kMIN, nvinfer1::OptProfileSelector::kOPT,
                         nvinfer1::OptProfileSelector::kMAX})
            profile->setDimensions(in_name, sel, nvinfer1::Dims4{1, 3, H, W});
        config->addOptimizationProfile(profile);
        std::fprintf(stderr, "[build] dynamic input -> profile fixed at 1x3x%dx%d\n", H, W);
    }

    std::unique_ptr<edge_ai::Int8EntropyCalibrator> calib;
    if (use_int8) {
        if (!builder->platformHasFastInt8())
            std::fprintf(stderr, "[build] warning: platform reports no fast INT8\n");
        config->setFlag(nvinfer1::BuilderFlag::kINT8);
        if (!cdir.empty()) {
            calib = std::make_unique<edge_ai::Int8EntropyCalibrator>(cdir, ccache, W, H, batch,
                                                                     in_name);
            config->setInt8Calibrator(calib.get());
        } else {
            std::fprintf(stderr,
                         "[build] --int8 without --calib-dir: relying on existing cache %s\n",
                         ccache.c_str());
        }
    }
    if (use_fp16 || use_int8) {
        if (builder->platformHasFastFp16()) config->setFlag(nvinfer1::BuilderFlag::kFP16);
    }

    std::fprintf(stderr, "[build] building engine (int8=%d fp16=%d) ... this can take minutes\n",
                 use_int8, use_fp16);
    TrtUnique<nvinfer1::IHostMemory> serialized(
        builder->buildSerializedNetwork(*network, *config));
    if (!serialized) {
        std::fprintf(stderr, "[build] buildSerializedNetwork failed\n");
        return 1;
    }

    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(serialized->data()), std::streamsize(serialized->size()));
    std::fprintf(stderr, "[build] wrote %s (%zu bytes)\n", out.c_str(), serialized->size());
    return 0;
}
