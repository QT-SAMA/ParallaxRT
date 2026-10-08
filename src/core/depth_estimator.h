#pragma once

#include <filesystem>
#include <memory>

#include <opencv2/core.hpp>

namespace parallaxrt {

enum class ExecutionProvider {
    Cpu,
    DirectML,
    Cuda
};

// CUDA builds use the NVIDIA provider by default; other builds retain the DirectML default.
constexpr ExecutionProvider defaultExecutionProvider() noexcept {
#ifdef PARALLAXRT_ENABLE_CUDA
    return ExecutionProvider::Cuda;
#else
    return ExecutionProvider::DirectML;
#endif
}

// Loads the first image input and first tensor output from an ONNX depth model.
// The returned depth is CV_32FC1 and retains the model's relative depth values.
class DepthEstimator {
public:
    explicit DepthEstimator(const std::filesystem::path& modelPath,
                            ExecutionProvider provider = defaultExecutionProvider(),
                            int dynamicInputSize = 518);
    ~DepthEstimator();

    DepthEstimator(DepthEstimator&&) noexcept;
    DepthEstimator& operator=(DepthEstimator&&) noexcept;
    DepthEstimator(const DepthEstimator&) = delete;
    DepthEstimator& operator=(const DepthEstimator&) = delete;

    cv::Mat infer(const cv::Mat& bgrFrame);
    const char* providerName() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace parallaxrt
