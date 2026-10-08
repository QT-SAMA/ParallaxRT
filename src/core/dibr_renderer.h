#pragma once

#include <utility>
#include <opencv2/core.hpp>

namespace parallaxrt {

enum class OutputMode {
    FullSbs,
    HalfSbs,
    Anaglyph,
    DepthMap,
    TopAndBottom
};

struct DibrParameters {
    float maxDisparity = 24.0f;
    float focusDepth = 0.5f;
    int inverseIterations = 1;
    bool enableTemporalSmoothing = true;
    float temporalAlpha = 0.85f;
    bool protectSubtitles = true;
    bool enableRgbDetail = true;
    float detailStrength = 0.22f;
    bool swapEyes = false;
};

// Resets temporal smoothing state across video playback sessions.
void resetDibrTemporalState();

// Robustly maps a model's relative depth range to [0,1].
cv::Mat normalizeDepth(const cv::Mat& rawDepth);

// Synthesizes the selected stereo format from a BGR frame and normalized depth.
cv::Mat renderStereo(const cv::Mat& bgrFrame,
                    const cv::Mat& normalizedDepth,
                    const DibrParameters& parameters,
                    OutputMode mode);

// Accepts the model's raw depth output and normalizes/upsamples it as part of rendering.
// CUDA builds execute normalization, temporal filtering and stereo reprojection on the GPU.
cv::Mat renderStereoFromRawDepth(const cv::Mat& bgrFrame,
                                const cv::Mat& rawDepth,
                                const DibrParameters& parameters,
                                OutputMode mode);

} // namespace parallaxrt
