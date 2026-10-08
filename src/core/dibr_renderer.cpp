#include "core/dibr_renderer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace parallaxrt {

#ifdef PARALLAXRT_ENABLE_CUDA
cv::Mat renderStereoCuda(const cv::Mat& bgrFrame,
                        const cv::Mat& rawDepth,
                        const DibrParameters& parameters,
                        OutputMode mode);
#endif

cv::Mat normalizeDepth(const cv::Mat& rawDepth) {
    if (rawDepth.empty() || rawDepth.type() != CV_32FC1) {
        throw std::invalid_argument("深度图必须是非空的 CV_32FC1 矩阵。");
    }

    cv::Mat finiteDepth = rawDepth.clone();
    for (int y = 0; y < finiteDepth.rows; ++y) {
        float* row = finiteDepth.ptr<float>(y);
        for (int x = 0; x < finiteDepth.cols; ++x) {
            if (!std::isfinite(row[x])) {
                row[x] = 0.0f;
            }
        }
    }

    double minValue = 0.0;
    double maxValue = 0.0;
    cv::minMaxLoc(finiteDepth, &minValue, &maxValue);
    if (maxValue - minValue < 1e-6) {
        return cv::Mat(finiteDepth.size(), CV_32FC1, cv::Scalar(0.5f));
    }
    finiteDepth.convertTo(finiteDepth, CV_32FC1,
                          1.0 / (maxValue - minValue),
                          -minValue / (maxValue - minValue));
    return finiteDepth;
}

cv::Mat renderStereo(const cv::Mat& bgrFrame,
                    const cv::Mat& normalizedDepth,
                    const DibrParameters& parameters,
                    const OutputMode mode) {
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3) {
        throw std::invalid_argument("渲染输入必须是非空的 8 位 BGR 图像。");
    }
    if (normalizedDepth.empty() || normalizedDepth.type() != CV_32FC1 ||
        normalizedDepth.size() != bgrFrame.size()) {
        throw std::invalid_argument("归一化深度图尺寸必须与视频帧一致，且类型为 CV_32FC1。");
    }
    if (!std::isfinite(parameters.maxDisparity) || parameters.maxDisparity < 0.0f ||
        !std::isfinite(parameters.focusDepth) || parameters.focusDepth < 0.0f ||
        parameters.focusDepth > 1.0f || parameters.inverseIterations < 1) {
        throw std::invalid_argument("DIBR 参数无效。");
    }

#ifdef PARALLAXRT_ENABLE_CUDA
    return renderStereoCuda(bgrFrame, normalizedDepth, parameters, mode);
#else

    if (mode == OutputMode::DepthMap) {
        cv::Mat depthGray(height, width, CV_8UC1);
        cv::parallel_for_(cv::Range(0, height), [&](const cv::Range& rows) {
            for (int y = rows.start; y < rows.end; ++y) {
                const float* depthRow = normalizedDepth.ptr<float>(y);
                const cv::Vec3b* bgrRow = bgrFrame.ptr<cv::Vec3b>(y);
                unsigned char* grayRow = depthGray.ptr<unsigned char>(y);
                for (int x = 0; x < width; ++x) {
                    float d = depthRow[x];
                    if (parameters.enableRgbDetail && parameters.detailStrength > 0.0f &&
                        x > 1 && x < width - 2 && y > 1 && y < height - 2) {
                        const auto& c = bgrRow[x];
                        const auto& l = bgrRow[x - 2];
                        const auto& r = bgrRow[x + 2];
                        const auto& t = bgrFrame.ptr<cv::Vec3b>(y - 2)[x];
                        const auto& b = bgrFrame.ptr<cv::Vec3b>(y + 2)[x];
                        const float lumC = 0.114f * c[0] + 0.587f * c[1] + 0.299f * c[2];
                        const float lumL = 0.114f * l[0] + 0.587f * l[1] + 0.299f * l[2];
                        const float lumR = 0.114f * r[0] + 0.587f * r[1] + 0.299f * r[2];
                        const float lumT = 0.114f * t[0] + 0.587f * t[1] + 0.299f * t[2];
                        const float lumB = 0.114f * b[0] + 0.587f * b[1] + 0.299f * b[2];
                        const float diff = (lumC - 0.25f * (lumL + lumR + lumT + lumB)) / 255.0f;
                        d = std::clamp(d + diff * parameters.detailStrength, 0.0f, 1.0f);
                    }
                    grayRow[x] = static_cast<unsigned char>(std::clamp(d * 255.0f, 0.0f, 255.0f));
                }
            }
        });
        cv::Mat result;
        cv::cvtColor(depthGray, result, cv::COLOR_GRAY2BGR);
        return result;
    }

    const int width = bgrFrame.cols;
    const int height = bgrFrame.rows;
    cv::Mat mapLeft(height, width, CV_32FC1);
    cv::Mat mapRight(height, width, CV_32FC1);
    cv::Mat mapY(height, width, CV_32FC1);
    const int iterations = std::clamp(parameters.inverseIterations, 1, 8);

    // Solve the inverse horizontal warp with a few fixed-point iterations.
    // This samples the source at each output pixel, avoiding forward-warp holes.
    cv::parallel_for_(cv::Range(0, height), [&](const cv::Range& rows) {
        for (int y = rows.start; y < rows.end; ++y) {
            const float* depth = normalizedDepth.ptr<float>(y);
            const cv::Vec3b* bgrRow = bgrFrame.ptr<cv::Vec3b>(y);
            float* leftMap = mapLeft.ptr<float>(y);
            float* rightMap = mapRight.ptr<float>(y);
            float* yMap = mapY.ptr<float>(y);
            const bool inBottomSubtitle = parameters.protectSubtitles && (y > static_cast<int>(height * 0.78f));
            for (int x = 0; x < width; ++x) {
                const float xPosition = static_cast<float>(x);
                float subDamp = 1.0f;
                if (parameters.protectSubtitles) {
                    if (inBottomSubtitle) {
                        subDamp = 0.35f;
                    } else if (x > 1 && x < width - 2) {
                        const auto& c = bgrRow[x];
                        const auto& l = bgrRow[x - 1];
                        const auto& r = bgrRow[x + 1];
                        const float lumC = 0.114f * c[0] + 0.587f * c[1] + 0.299f * c[2];
                        const float lumL = 0.114f * l[0] + 0.587f * l[1] + 0.299f * l[2];
                        const float lumR = 0.114f * r[0] + 0.587f * r[1] + 0.299f * r[2];
                        if (std::max(std::abs(lumC - lumL), std::abs(lumR - lumC)) > 50.0f) {
                            subDamp = 0.55f;
                        }
                    }
                }
                float relief = 0.0f;
                if (parameters.enableRgbDetail && parameters.detailStrength > 0.0f &&
                    x > 1 && x < width - 2 && y > 1 && y < height - 2) {
                    const auto& c = bgrRow[x];
                    const auto& l = bgrRow[x - 2];
                    const auto& r = bgrRow[x + 2];
                    const auto& t = bgrFrame.ptr<cv::Vec3b>(y - 2)[x];
                    const auto& b = bgrFrame.ptr<cv::Vec3b>(y + 2)[x];
                    const float lumC = 0.114f * c[0] + 0.587f * c[1] + 0.299f * c[2];
                    const float lumL = 0.114f * l[0] + 0.587f * l[1] + 0.299f * l[2];
                    const float lumR = 0.114f * r[0] + 0.587f * r[1] + 0.299f * r[2];
                    const float lumT = 0.114f * t[0] + 0.587f * t[1] + 0.299f * t[2];
                    const float lumB = 0.114f * b[0] + 0.587f * b[1] + 0.299f * b[2];
                    const float diff = (lumC - 0.25f * (lumL + lumR + lumT + lumB)) / 255.0f;
                    relief = diff * parameters.detailStrength * subDamp;
                }
                const float depthVal = std::clamp(depth[x] + relief, 0.0f, 1.0f);
                if (iterations == 1) {
                    const float shift =
                        (depthVal - parameters.focusDepth) * parameters.maxDisparity * subDamp;
                    leftMap[x] = xPosition + 0.5f * shift;
                    rightMap[x] = xPosition - 0.5f * shift;
                } else {
                    float leftSourceX = xPosition;
                    float rightSourceX = xPosition;
                    for (int i = 0; i < iterations; ++i) {
                        const int leftDepthX = std::clamp(
                            static_cast<int>(std::lround(leftSourceX)), 0, width - 1);
                        const int rightDepthX = std::clamp(
                            static_cast<int>(std::lround(rightSourceX)), 0, width - 1);
                        const float leftShift = (std::clamp(depth[leftDepthX] + relief, 0.0f, 1.0f) - parameters.focusDepth) *
                                                parameters.maxDisparity * subDamp;
                        const float rightShift = (std::clamp(depth[rightDepthX] + relief, 0.0f, 1.0f) - parameters.focusDepth) *
                                                 parameters.maxDisparity * subDamp;
                        leftSourceX = xPosition + 0.5f * leftShift;
                        rightSourceX = xPosition - 0.5f * rightShift;
                    }
                    leftMap[x] = leftSourceX;
                    rightMap[x] = rightSourceX;
                }
                yMap[x] = static_cast<float>(y);
            }
        }
    });

    cv::Mat leftEye;
    cv::Mat rightEye;
    cv::remap(bgrFrame, leftEye, mapLeft, mapY, cv::INTER_LINEAR,
              cv::BORDER_REPLICATE);
    cv::remap(bgrFrame, rightEye, mapRight, mapY, cv::INTER_LINEAR,
              cv::BORDER_REPLICATE);

    if (!parameters.swapEyes) {
        std::swap(leftEye, rightEye);
    }

    if (mode == OutputMode::Anaglyph) {
        cv::Mat sources[] = {leftEye, rightEye};
        cv::Mat anaglyph(height, width, CV_8UC3);
        constexpr int channelMap[] = {3, 0, 4, 1, 2, 2};
        cv::mixChannels(sources, 2, &anaglyph, 1, channelMap, 3);
        return anaglyph;
    }

    if (mode == OutputMode::TopAndBottom) {
        const int eyeHeight = std::max(1, height / 2);
        cv::resize(leftEye, leftEye, cv::Size(width, eyeHeight), 0.0, 0.0, cv::INTER_AREA);
        cv::resize(rightEye, rightEye, cv::Size(width, eyeHeight), 0.0, 0.0, cv::INTER_AREA);
        cv::Mat tab;
        cv::vconcat(leftEye, rightEye, tab);
        return tab;
    }

    if (mode == OutputMode::HalfSbs) {
        const int eyeWidth = std::max(1, width / 2);
        cv::resize(leftEye, leftEye, cv::Size(eyeWidth, height), 0.0, 0.0, cv::INTER_AREA);
        cv::resize(rightEye, rightEye, cv::Size(eyeWidth, height), 0.0, 0.0, cv::INTER_AREA);
    }

    cv::Mat sbs;
    cv::hconcat(leftEye, rightEye, sbs);
    return sbs;
#endif
}

#ifndef PARALLAXRT_ENABLE_CUDA
void resetDibrTemporalState() {
}
#endif

cv::Mat renderStereoFromRawDepth(const cv::Mat& bgrFrame,
                                const cv::Mat& rawDepth,
                                const DibrParameters& parameters,
                                const OutputMode mode) {
#ifdef PARALLAXRT_ENABLE_CUDA
    return renderStereoCuda(bgrFrame, rawDepth, parameters, mode);
#else
    cv::Mat depth = normalizeDepth(rawDepth);
    if (depth.size() != bgrFrame.size()) {
        cv::resize(depth, depth, bgrFrame.size(), 0.0, 0.0, cv::INTER_LINEAR);
    }
    return renderStereo(bgrFrame, depth, parameters, mode);
#endif
}

} // namespace parallaxrt
