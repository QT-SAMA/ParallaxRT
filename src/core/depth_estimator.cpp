#include "core/depth_estimator.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#ifdef PARALLAXRT_ENABLE_DIRECTML
#include <dml_provider_factory.h>
#endif

namespace parallaxrt {
namespace {

Ort::SessionOptions makeSessionOptions(const ExecutionProvider provider) {
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (provider == ExecutionProvider::DirectML) {
#ifdef PARALLAXRT_ENABLE_DIRECTML
        // DirectML requires sequential graph execution and disabled memory-pattern reuse.
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.DisableMemPattern();

        const OrtApi& api = Ort::GetApi();
        const OrtDmlApi* dmlApi = nullptr;
        Ort::ThrowOnError(api.GetExecutionProviderApi(
            "DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));
        if (dmlApi == nullptr) {
            throw std::runtime_error("当前 ONNX Runtime 未提供 DirectML 执行提供器。");
        }
        Ort::ThrowOnError(dmlApi->SessionOptionsAppendExecutionProvider_DML(options, 0));
#else
        throw std::runtime_error(
            "此构建未启用 DirectML；请使用 DirectML 版 ONNX Runtime 重新配置，或传入 --provider cpu。");
#endif
    }
    if (provider == ExecutionProvider::Cuda) {
#ifdef PARALLAXRT_ENABLE_CUDA
        OrtCUDAProviderOptions cudaOptions{};
        cudaOptions.device_id = 0;
        options.AppendExecutionProvider_CUDA(cudaOptions);
#else
        throw std::runtime_error(
            "此构建未启用 CUDA；请安装 CUDA 版 ONNX Runtime 与 CUDA Toolkit 后重新配置 PARALLAXRT_ENABLE_CUDA。 ");
#endif
    }
    return options;
}

} // namespace

struct DepthEstimator::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ParallaxRT"};
    Ort::Session session;
    Ort::MemoryInfo memoryInfo{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    std::string inputName;
    std::string outputName;
    int inputWidth = 518;
    int inputHeight = 518;
    ExecutionProvider provider;
    cv::Mat resizedBgr;
    std::array<cv::Mat, 3> bgrChannels;
    std::vector<float> inputBuffer;

    Impl(const std::filesystem::path& modelPath,
         const ExecutionProvider selectedProvider,
         const int dynamicInputSize)
        : session(env, modelPath.c_str(), makeSessionOptions(selectedProvider)),
          provider(selectedProvider) {
        if (dynamicInputSize <= 0) {
            throw std::invalid_argument("动态输入边长必须大于 0。");
        }
        if (session.GetInputCount() < 1 || session.GetOutputCount() < 1) {
            throw std::runtime_error("ONNX 模型必须至少包含一个输入和一个输出。");
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAllocated = session.GetInputNameAllocated(0, allocator);
        auto outputNameAllocated = session.GetOutputNameAllocated(0, allocator);
        inputName = inputNameAllocated.get();
        outputName = outputNameAllocated.get();

        const auto inputInfo = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto inputShape = inputInfo.GetShape();
        if (inputShape.size() != 4) {
            throw std::runtime_error("模型输入需要是 [N,C,H,W] 四维张量。");
        }
        if (inputShape[1] > 0 && inputShape[1] != 3) {
            throw std::runtime_error("模型输入通道数不是 3，当前只支持 RGB 深度模型。");
        }
        inputHeight = inputShape[2] > 0 ? static_cast<int>(inputShape[2]) : dynamicInputSize;
        inputWidth = inputShape[3] > 0 ? static_cast<int>(inputShape[3]) : dynamicInputSize;
        resizedBgr.create(inputHeight, inputWidth, CV_8UC3);
        for (auto& channel : bgrChannels) {
            channel.create(inputHeight, inputWidth, CV_8UC1);
        }
        inputBuffer.resize(3 * static_cast<size_t>(inputWidth) * inputHeight);
    }
};

DepthEstimator::DepthEstimator(const std::filesystem::path& modelPath,
                               const ExecutionProvider provider,
                               const int dynamicInputSize)
    : impl_(std::make_unique<Impl>(modelPath, provider, dynamicInputSize)) {}

DepthEstimator::~DepthEstimator() = default;
DepthEstimator::DepthEstimator(DepthEstimator&&) noexcept = default;
DepthEstimator& DepthEstimator::operator=(DepthEstimator&&) noexcept = default;

const char* DepthEstimator::providerName() const noexcept {
    if (!impl_) {
        return "Unknown";
    }
    switch (impl_->provider) {
        case ExecutionProvider::Cuda: return "CUDA";
        case ExecutionProvider::DirectML: return "DirectML";
        case ExecutionProvider::Cpu: return "CPU";
    }
    return "Unknown";
}

cv::Mat DepthEstimator::infer(const cv::Mat& bgrFrame) {
    if (!impl_) {
        throw std::logic_error("深度估计器已被移动，无法继续推理。");
    }
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3) {
        throw std::invalid_argument("输入帧必须是非空的 8 位 BGR 图像。");
    }

    cv::resize(bgrFrame, impl_->resizedBgr, cv::Size(impl_->inputWidth, impl_->inputHeight),
               0.0, 0.0, cv::INTER_LINEAR);
    cv::split(impl_->resizedBgr, impl_->bgrChannels.data());

    constexpr std::array<float, 3> mean{0.485f, 0.456f, 0.406f};
    constexpr std::array<float, 3> stddev{0.229f, 0.224f, 0.225f};
    const size_t planeSize = static_cast<size_t>(impl_->inputWidth) * impl_->inputHeight;
    for (int channel = 0; channel < 3; ++channel) {
        cv::Mat normalizedPlane(impl_->inputHeight, impl_->inputWidth, CV_32FC1,
                                impl_->inputBuffer.data() +
                                    static_cast<size_t>(channel) * planeSize);
        const double scale = 1.0 / (255.0 * stddev[channel]);
        const double offset = -mean[channel] / stddev[channel];
        // OpenCV's vectorized conversion writes directly into the reusable NCHW buffer.
        impl_->bgrChannels[2 - channel].convertTo(
            normalizedPlane, CV_32FC1, scale, offset);
    }

    const std::array<int64_t, 4> inputShape{1, 3, impl_->inputHeight, impl_->inputWidth};
    auto inputTensor = Ort::Value::CreateTensor<float>(
        impl_->memoryInfo, impl_->inputBuffer.data(), impl_->inputBuffer.size(),
        inputShape.data(), inputShape.size());
    const char* inputNames[] = {impl_->inputName.c_str()};
    const char* outputNames[] = {impl_->outputName.c_str()};
    auto outputs = impl_->session.Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1,
                                      outputNames, 1);
    if (outputs.empty() || !outputs.front().IsTensor()) {
        throw std::runtime_error("ONNX 模型没有返回深度张量。");
    }

    const auto outputInfo = outputs.front().GetTensorTypeAndShapeInfo();
    if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("当前只支持 FP32 ONNX 深度输出。");
    }
    const auto outputShape = outputInfo.GetShape();
    if (outputShape.size() < 2 || outputShape.back() <= 0 || outputShape[outputShape.size() - 2] <= 0) {
        throw std::runtime_error("无法识别模型输出的深度图宽高。");
    }
    const int outputHeight = static_cast<int>(outputShape[outputShape.size() - 2]);
    const int outputWidth = static_cast<int>(outputShape.back());
    const size_t expectedValues = static_cast<size_t>(outputHeight) * outputWidth;
    if (outputInfo.GetElementCount() < expectedValues) {
        throw std::runtime_error("模型输出张量元素数量与深度图尺寸不匹配。");
    }

    const float* outputData = outputs.front().GetTensorData<float>();
    return cv::Mat(outputHeight, outputWidth, CV_32FC1,
                   const_cast<float*>(outputData)).clone();
}

} // namespace parallaxrt
