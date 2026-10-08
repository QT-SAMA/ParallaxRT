#include "core/dibr_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <cuda_runtime.h>
#include <math_constants.h>
#include <opencv2/core.hpp>

namespace parallaxrt {
namespace {

constexpr int kThreadsPerBlock = 256;

void checkCuda(const cudaError_t error, const char* operation) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(error));
    }
}

template <typename T>
void reserveDeviceBuffer(T*& buffer, size_t& capacity, const size_t required) {
    if (required <= capacity) {
        return;
    }
    if (buffer != nullptr) {
        checkCuda(cudaFree(buffer), "cudaFree");
        buffer = nullptr;
        capacity = 0;
    }
    checkCuda(cudaMalloc(reinterpret_cast<void**>(&buffer), required * sizeof(T)), "cudaMalloc");
    capacity = required;
}

struct DeviceScratch {
    unsigned char* frame = nullptr;
    unsigned char* prevFrame = nullptr;
    float* rawDepth = nullptr;
    float* partialMin = nullptr;
    float* partialMax = nullptr;
    float* minMax = nullptr;
    float* smoothedMinMax = nullptr;
    float* prevDepth = nullptr;
    unsigned char* output = nullptr;
    size_t frameCapacity = 0;
    size_t prevFrameCapacity = 0;
    size_t depthCapacity = 0;
    size_t partialMinCapacity = 0;
    size_t partialMaxCapacity = 0;
    size_t minMaxCapacity = 0;
    size_t smoothedMinMaxCapacity = 0;
    size_t prevDepthCapacity = 0;
    size_t outputCapacity = 0;
    bool hasHistory = false;

    ~DeviceScratch() {
        if (frame != nullptr) cudaFree(frame);
        if (prevFrame != nullptr) cudaFree(prevFrame);
        if (rawDepth != nullptr) cudaFree(rawDepth);
        if (partialMin != nullptr) cudaFree(partialMin);
        if (partialMax != nullptr) cudaFree(partialMax);
        if (minMax != nullptr) cudaFree(minMax);
        if (smoothedMinMax != nullptr) cudaFree(smoothedMinMax);
        if (prevDepth != nullptr) cudaFree(prevDepth);
        if (output != nullptr) cudaFree(output);
    }

    void resetHistory() {
        hasHistory = false;
    }

    void reserve(const size_t frameBytes,
                 const size_t depthValues,
                 const size_t partialValues,
                 const size_t outputBytes) {
        reserveDeviceBuffer(frame, frameCapacity, frameBytes);
        reserveDeviceBuffer(prevFrame, prevFrameCapacity, frameBytes);
        reserveDeviceBuffer(rawDepth, depthCapacity, depthValues);
        reserveDeviceBuffer(partialMin, partialMinCapacity, partialValues);
        reserveDeviceBuffer(partialMax, partialMaxCapacity, partialValues);
        reserveDeviceBuffer(minMax, minMaxCapacity, 2);
        reserveDeviceBuffer(smoothedMinMax, smoothedMinMaxCapacity, 2);
        reserveDeviceBuffer(prevDepth, prevDepthCapacity, depthValues);
        reserveDeviceBuffer(output, outputCapacity, outputBytes);
    }
};

thread_local DeviceScratch g_cudaScratch;

__global__ void reduceMinMaxKernel(const float* values,
                                   const size_t count,
                                   float* partialMin,
                                   float* partialMax) {
    extern __shared__ float shared[];
    float* minima = shared;
    float* maxima = shared + blockDim.x;

    const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    float value = index < count ? values[index] : 0.0f;
    if (!isfinite(value)) {
        value = 0.0f;
    }
    minima[threadIdx.x] = index < count ? value : CUDART_INF_F;
    maxima[threadIdx.x] = index < count ? value : -CUDART_INF_F;
    __syncthreads();

    for (unsigned int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            minima[threadIdx.x] = fminf(minima[threadIdx.x], minima[threadIdx.x + stride]);
            maxima[threadIdx.x] = fmaxf(maxima[threadIdx.x], maxima[threadIdx.x + stride]);
        }
        __syncthreads();
    }

    if (threadIdx.x == 0) {
        partialMin[blockIdx.x] = minima[0];
        partialMax[blockIdx.x] = maxima[0];
    }
}

__global__ void finishMinMaxKernel(const float* partialMin,
                                   const float* partialMax,
                                   const size_t partialCount,
                                   float* result) {
    extern __shared__ float shared[];
    float* minima = shared;
    float* maxima = shared + blockDim.x;
    float localMin = CUDART_INF_F;
    float localMax = -CUDART_INF_F;
    for (size_t i = threadIdx.x; i < partialCount; i += blockDim.x) {
        localMin = fminf(localMin, partialMin[i]);
        localMax = fmaxf(localMax, partialMax[i]);
    }
    minima[threadIdx.x] = localMin;
    maxima[threadIdx.x] = localMax;
    __syncthreads();

    for (unsigned int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            minima[threadIdx.x] = fminf(minima[threadIdx.x], minima[threadIdx.x + stride]);
            maxima[threadIdx.x] = fmaxf(maxima[threadIdx.x], maxima[threadIdx.x + stride]);
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        result[0] = isfinite(minima[0]) ? minima[0] : 0.0f;
        result[1] = isfinite(maxima[0]) ? maxima[0] : 0.0f;
    }
}

__global__ void applyTemporalMinMaxKernel(const float* currentMinMax,
                                          float* smoothedMinMax,
                                          const float alpha,
                                          const bool hasHistory) {
    if (threadIdx.x == 0) {
        const float curMin = currentMinMax[0];
        const float curMax = currentMinMax[1];
        if (!hasHistory || alpha <= 0.0f) {
            smoothedMinMax[0] = curMin;
            smoothedMinMax[1] = curMax;
            return;
        }
        const float curRange = curMax - curMin;
        const float prevRange = smoothedMinMax[1] - smoothedMinMax[0];
        if (fabsf(curRange - prevRange) > 0.55f * fmaxf(prevRange, 1e-4f)) {
            smoothedMinMax[0] = curMin;
            smoothedMinMax[1] = curMax;
        } else {
            smoothedMinMax[0] = alpha * smoothedMinMax[0] + (1.0f - alpha) * curMin;
            smoothedMinMax[1] = alpha * smoothedMinMax[1] + (1.0f - alpha) * curMax;
        }
    }
}

__global__ void blendDepthKernel(float* currentDepth,
                                 float* prevDepth,
                                 const size_t count,
                                 const float blendFactor,
                                 const bool hasHistory) {
    const size_t idx = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (idx >= count) return;
    float curVal = currentDepth[idx];
    if (!isfinite(curVal)) curVal = 0.0f;
    if (hasHistory && blendFactor > 0.0f) {
        const float prevVal = prevDepth[idx];
        if (isfinite(prevVal)) {
            curVal = blendFactor * prevVal + (1.0f - blendFactor) * curVal;
        }
    }
    currentDepth[idx] = curVal;
    prevDepth[idx] = curVal;
}

__device__ float clampFloat(const float value, const float low, const float high) {
    return fminf(fmaxf(value, low), high);
}

__device__ float finiteDepthValue(const float value) {
    return isfinite(value) ? value : 0.0f;
}

__device__ float computeLuminanceAt(const unsigned char* frame,
                                    const int frameWidth,
                                    const int x,
                                    const int y) {
    const size_t offset = (static_cast<size_t>(y) * frameWidth + x) * 3;
    return 0.114f * static_cast<float>(frame[offset]) +
           0.587f * static_cast<float>(frame[offset + 1]) +
           0.299f * static_cast<float>(frame[offset + 2]);
}

__device__ float guidedNormalizedDepthAt(const float* rawDepth,
                                         const float* minMax,
                                         const unsigned char* frame,
                                         const int depthWidth,
                                         const int depthHeight,
                                         const int frameWidth,
                                         const int frameHeight,
                                         const float frameX,
                                         const int frameY,
                                         const bool enableRgbGuide) {
    const float depthX = clampFloat(
        (frameX + 0.5f) * static_cast<float>(depthWidth) / static_cast<float>(frameWidth) - 0.5f,
        0.0f, static_cast<float>(depthWidth - 1));
    const float depthY = clampFloat(
        (static_cast<float>(frameY) + 0.5f) * static_cast<float>(depthHeight) /
                static_cast<float>(frameHeight) - 0.5f,
        0.0f, static_cast<float>(depthHeight - 1));
    const int x0 = static_cast<int>(floorf(depthX));
    const int y0 = static_cast<int>(floorf(depthY));
    const int x1 = min(x0 + 1, depthWidth - 1);
    const int y1 = min(y0 + 1, depthHeight - 1);
    const float tx = depthX - static_cast<float>(x0);
    const float ty = depthY - static_cast<float>(y0);

    const float d00 = finiteDepthValue(rawDepth[y0 * depthWidth + x0]);
    const float d10 = finiteDepthValue(rawDepth[y0 * depthWidth + x1]);
    const float d01 = finiteDepthValue(rawDepth[y1 * depthWidth + x0]);
    const float d11 = finiteDepthValue(rawDepth[y1 * depthWidth + x1]);

    float value = 0.0f;
    if (enableRgbGuide) {
        const int ifX = min(max(static_cast<int>(floorf(frameX + 0.5f)), 0), frameWidth - 1);
        const float lumCenter = computeLuminanceAt(frame, frameWidth, ifX, frameY);

        const int fx0 = min(max(static_cast<int>((static_cast<float>(x0) + 0.5f) * frameWidth / depthWidth), 0), frameWidth - 1);
        const int fx1 = min(max(static_cast<int>((static_cast<float>(x1) + 0.5f) * frameWidth / depthWidth), 0), frameWidth - 1);
        const int fy0 = min(max(static_cast<int>((static_cast<float>(y0) + 0.5f) * frameHeight / depthHeight), 0), frameHeight - 1);
        const int fy1 = min(max(static_cast<int>((static_cast<float>(y1) + 0.5f) * frameHeight / depthHeight), 0), frameHeight - 1);

        const float l00 = computeLuminanceAt(frame, frameWidth, fx0, fy0);
        const float l10 = computeLuminanceAt(frame, frameWidth, fx1, fy0);
        const float l01 = computeLuminanceAt(frame, frameWidth, fx0, fy1);
        const float l11 = computeLuminanceAt(frame, frameWidth, fx1, fy1);

        constexpr float kInvTwoSigmaSq = 0.0005f;
        const float diff00 = lumCenter - l00;
        const float diff10 = lumCenter - l10;
        const float diff01 = lumCenter - l01;
        const float diff11 = lumCenter - l11;

        const float w00 = (1.0f - tx) * (1.0f - ty) * __expf(-diff00 * diff00 * kInvTwoSigmaSq);
        const float w10 = tx * (1.0f - ty) * __expf(-diff10 * diff10 * kInvTwoSigmaSq);
        const float w01 = (1.0f - tx) * ty * __expf(-diff01 * diff01 * kInvTwoSigmaSq);
        const float w11 = tx * ty * __expf(-diff11 * diff11 * kInvTwoSigmaSq);

        const float totalWeight = w00 + w10 + w01 + w11;
        if (totalWeight > 1e-4f) {
            value = (w00 * d00 + w10 * d10 + w01 * d01 + w11 * d11) / totalWeight;
        } else {
            const float top = d00 * (1.0f - tx) + d10 * tx;
            const float bottom = d01 * (1.0f - tx) + d11 * tx;
            value = top * (1.0f - ty) + bottom * ty;
        }
    } else {
        const float top = d00 * (1.0f - tx) + d10 * tx;
        const float bottom = d01 * (1.0f - tx) + d11 * tx;
        value = top * (1.0f - ty) + bottom * ty;
    }

    const float range = minMax[1] - minMax[0];
    if (!isfinite(range) || range < 1e-6f) {
        return 0.5f;
    }
    return clampFloat((value - minMax[0]) / range, 0.0f, 1.0f);
}

__device__ float computeRgbRelief(const unsigned char* frame,
                                  const int frameWidth,
                                  const int frameHeight,
                                  const int x,
                                  const int y) {
    const int xL = max(0, x - 2);
    const int xR = min(frameWidth - 1, x + 2);
    const int yT = max(0, y - 2);
    const int yB = min(frameHeight - 1, y + 2);

    const float lumC = computeLuminanceAt(frame, frameWidth, x, y);
    const float lumL = computeLuminanceAt(frame, frameWidth, xL, y);
    const float lumR = computeLuminanceAt(frame, frameWidth, xR, y);
    const float lumT = computeLuminanceAt(frame, frameWidth, x, yT);
    const float lumB = computeLuminanceAt(frame, frameWidth, x, yB);

    const float avg = 0.25f * (lumL + lumR + lumT + lumB);
    const float diff = lumC - avg;

    float relief = diff / 255.0f;
    if (fabsf(relief) < 0.012f) {
        relief = 0.0f;
    } else {
        relief = copysignf(fabsf(relief) - 0.012f, relief) * 1.15f;
    }
    return clampFloat(relief, -0.30f, 0.30f);
}

__device__ float warpSourceX(const float targetX,
                             const int y,
                             const bool leftEye,
                             const unsigned char* frame,
                             const float* rawDepth,
                             const float* minMax,
                             const int depthWidth,
                             const int depthHeight,
                             const int frameWidth,
                             const int frameHeight,
                             const float focusDepth,
                             const float maxDisparity,
                             const int iterations,
                             const bool protectSubtitles,
                             const bool enableRgbDetail,
                             const float detailStrength,
                             float* outOcclusionHole = nullptr) {
    float subtitleDamp = 1.0f;
    if (protectSubtitles) {
        const int ix = min(max(static_cast<int>(floorf(targetX + 0.5f)), 1), frameWidth - 2);
        const float lumL = computeLuminanceAt(frame, frameWidth, ix - 1, y);
        const float lumC = computeLuminanceAt(frame, frameWidth, ix, y);
        const float lumR = computeLuminanceAt(frame, frameWidth, ix + 1, y);
        const float contrast = fmaxf(fabsf(lumC - lumL), fabsf(lumR - lumC));
        const bool inBottomSubtitleZone = (y > static_cast<int>(frameHeight * 0.78f));
        if (contrast > 50.0f || (inBottomSubtitleZone && contrast > 22.0f)) {
            subtitleDamp = inBottomSubtitleZone ? 0.35f : 0.55f;
        }
    }

    float sourceX = targetX;
    float prevShift = 0.0f;
    float maxGrad = 0.0f;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int depthSampleX = min(max(static_cast<int>(floorf(sourceX + 0.5f)), 0),
                                     frameWidth - 1);
        float depth = guidedNormalizedDepthAt(
            rawDepth, minMax, frame, depthWidth, depthHeight,
            frameWidth, frameHeight, static_cast<float>(depthSampleX), y,
            enableRgbDetail);

        // --- 优化1: 前景边缘深度腐蚀与遮挡抑制 (Boundary Edge Erosion & Occlusion Suppression) ---
        // 动漫与静止物体在边缘有非常清晰的轮廓线（黑线/高对比边界）。
        // 当视差较大时，如果采样点横跨突变边缘，会将前景物体边缘的像素连带拉伸到背景区域造成重叠边/重影。
        // 检测左右邻域梯度：
        if (depthSampleX >= 3 && depthSampleX <= frameWidth - 4) {
            const float dL1 = guidedNormalizedDepthAt(
                rawDepth, minMax, frame, depthWidth, depthHeight,
                frameWidth, frameHeight, static_cast<float>(depthSampleX - 2), y, false);
            const float dR1 = guidedNormalizedDepthAt(
                rawDepth, minMax, frame, depthWidth, depthHeight,
                frameWidth, frameHeight, static_cast<float>(depthSampleX + 2), y, false);
            const float gradL = depth - dL1;
            const float gradR = depth - dR1;
            const float curGrad = fmaxf(fabsf(gradL), fabsf(gradR));
            if (curGrad > maxGrad) {
                maxGrad = curGrad;
            }

            // 动漫/高反差边界的重影主要发生在背景侧被误当成前景拉伸，或前景边界向背景外溢。
            // 当左眼向右看或右眼向左看，空洞侧应严格使用背景深度（较小深度），杜绝前景边缘色块外溢：
            if (gradL > 0.12f && leftEye) {
                depth = dL1; // 强边缘遮挡侧：直接吸附至背景，消除前景边缘外拉重影
            } else if (gradR > 0.12f && !leftEye) {
                depth = dR1;
            } else if (gradL > 0.10f && !leftEye) {
                depth = depth - 0.25f * gradL; // 前景侧做边缘平滑收缩
            } else if (gradR > 0.10f && leftEye) {
                depth = depth - 0.25f * gradR;
            }
        }

        if (enableRgbDetail && detailStrength > 0.0f) {
            const float relief = computeRgbRelief(
                frame, frameWidth, frameHeight, depthSampleX, y);
            depth = clampFloat(depth + relief * detailStrength * subtitleDamp, 0.0f, 1.0f);
        }

        float shift = (depth - focusDepth) * maxDisparity * subtitleDamp;
        if (iteration > 0) {
            shift = clampFloat(shift, prevShift - 2.0f, prevShift + 2.0f);
        }
        prevShift = shift;
        sourceX = targetX + (leftEye ? 0.5f : -0.5f) * shift;
    }

    if (outOcclusionHole != nullptr) {
        // 梯度突变 > 0.15 且视差拉伸幅度较大时判定为遮挡空洞
        *outOcclusionHole = (maxGrad > 0.15f && fabsf(prevShift) > 2.0f)
            ? clampFloat((maxGrad - 0.15f) * 3.0f, 0.0f, 1.0f)
            : 0.0f;
    }

    return sourceX;
}

__device__ void sampleBgr(const unsigned char* frame,
                          const int width,
                          const int height,
                          const float x,
                          const int y,
                          unsigned char* pixel) {
    const float clampedX = clampFloat(x, 0.0f, static_cast<float>(width - 1));
    const int x0 = static_cast<int>(floorf(clampedX));
    const int x1 = min(x0 + 1, width - 1);
    const float tx = clampedX - static_cast<float>(x0);
    const size_t first = (static_cast<size_t>(y) * width + x0) * 3;
    const size_t second = (static_cast<size_t>(y) * width + x1) * 3;
    for (int channel = 0; channel < 3; ++channel) {
        const float value = frame[first + channel] * (1.0f - tx) + frame[second + channel] * tx;
        pixel[channel] = static_cast<unsigned char>(__float2int_rn(value));
    }
    (void)height;
}

__global__ void renderKernel(const unsigned char* frame,
                             const unsigned char* prevFrame,
                             const bool hasHistory,
                             const float* rawDepth,
                             const float* minMax,
                             unsigned char* output,
                             const int frameWidth,
                             const int frameHeight,
                             const int depthWidth,
                             const int depthHeight,
                             const int outputWidth,
                             const int outputHeight,
                             const int mode,
                             const float focusDepth,
                             const float maxDisparity,
                             const int iterations,
                             const bool protectSubtitles,
                             const bool enableRgbDetail,
                             const float detailStrength,
                             const bool swapEyes) {
    const size_t pixelIndex = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const size_t pixelCount = static_cast<size_t>(outputWidth) * outputHeight;
    if (pixelIndex >= pixelCount) {
        return;
    }

    const int outX = static_cast<int>(pixelIndex % outputWidth);
    const int y = static_cast<int>(pixelIndex / outputWidth);

    if (mode == static_cast<int>(OutputMode::DepthMap)) {
        float depth = guidedNormalizedDepthAt(
            rawDepth, minMax, frame, depthWidth, depthHeight,
            frameWidth, frameHeight, static_cast<float>(outX), y,
            enableRgbDetail);
        if (enableRgbDetail && detailStrength > 0.0f) {
            float relief = computeRgbRelief(frame, frameWidth, frameHeight, outX, y);
            if (protectSubtitles) {
                const int ix = min(max(outX, 1), frameWidth - 2);
                const float lumL = computeLuminanceAt(frame, frameWidth, ix - 1, y);
                const float lumC = computeLuminanceAt(frame, frameWidth, ix, y);
                const float lumR = computeLuminanceAt(frame, frameWidth, ix + 1, y);
                const float contrast = fmaxf(fabsf(lumC - lumL), fabsf(lumR - lumC));
                const bool inBottomSubtitleZone = (y > static_cast<int>(frameHeight * 0.78f));
                if (contrast > 50.0f || (inBottomSubtitleZone && contrast > 22.0f)) {
                    relief *= (inBottomSubtitleZone ? 0.35f : 0.55f);
                }
            }
            depth = clampFloat(depth + relief * detailStrength, 0.0f, 1.0f);
        }
        const unsigned char val = static_cast<unsigned char>(__float2int_rn(depth * 255.0f));
        const size_t outputOffset = pixelIndex * 3;
        output[outputOffset] = val;
        output[outputOffset + 1] = val;
        output[outputOffset + 2] = val;
        return;
    }

    if (mode == static_cast<int>(OutputMode::Anaglyph)) {
        const float targetX = static_cast<float>(outX);
        float holeL = 0.0f;
        float holeR = 0.0f;
        const float leftX = warpSourceX(targetX, y, swapEyes, frame, rawDepth, minMax,
                                        depthWidth, depthHeight, frameWidth, frameHeight,
                                        focusDepth, maxDisparity, iterations, protectSubtitles,
                                        enableRgbDetail, detailStrength, &holeL);
        const float rightX = warpSourceX(targetX, y, !swapEyes, frame, rawDepth, minMax,
                                         depthWidth, depthHeight, frameWidth, frameHeight,
                                         focusDepth, maxDisparity, iterations, protectSubtitles,
                                         enableRgbDetail, detailStrength, &holeR);
        unsigned char left[3];
        unsigned char right[3];
        sampleBgr(frame, frameWidth, frameHeight, leftX, y, left);
        sampleBgr(frame, frameWidth, frameHeight, rightX, y, right);

        // 时序修补：空洞处融合上一帧背景
        if (hasHistory && prevFrame != nullptr) {
            if (holeL > 0.05f) {
                unsigned char hist[3];
                sampleBgr(prevFrame, frameWidth, frameHeight, leftX, y, hist);
                for (int c = 0; c < 3; ++c) {
                    left[c] = static_cast<unsigned char>(__float2int_rn(
                        (1.0f - holeL * 0.65f) * left[c] + (holeL * 0.65f) * hist[c]));
                }
            }
            if (holeR > 0.05f) {
                unsigned char hist[3];
                sampleBgr(prevFrame, frameWidth, frameHeight, rightX, y, hist);
                for (int c = 0; c < 3; ++c) {
                    right[c] = static_cast<unsigned char>(__float2int_rn(
                        (1.0f - holeR * 0.65f) * right[c] + (holeR * 0.65f) * hist[c]));
                }
            }
        }

        const size_t outputOffset = pixelIndex * 3;
        output[outputOffset] = right[0];
        output[outputOffset + 1] = right[1];
        output[outputOffset + 2] = left[2];
        return;
    }

    if (mode == static_cast<int>(OutputMode::TopAndBottom)) {
        const int eyeHeight = max(1, frameHeight / 2);
        const bool leftEye = !((y < eyeHeight) ^ swapEyes);
        const int localY = (y < eyeHeight) ? y : y - eyeHeight;
        const float targetY = (static_cast<float>(localY) + 0.5f) * static_cast<float>(frameHeight) /
                                  static_cast<float>(eyeHeight) - 0.5f;
        const int srcY = min(max(__float2int_rn(targetY), 0), frameHeight - 1);
        const float targetX = static_cast<float>(outX);
        float hole = 0.0f;
        const float sourceX = warpSourceX(targetX, srcY, leftEye, frame, rawDepth, minMax,
                                          depthWidth, depthHeight, frameWidth, frameHeight,
                                          focusDepth, maxDisparity, iterations, protectSubtitles,
                                          enableRgbDetail, detailStrength, &hole);
        unsigned char pixel[3];
        sampleBgr(frame, frameWidth, frameHeight, sourceX, srcY, pixel);
        if (hasHistory && prevFrame != nullptr && hole > 0.05f) {
            unsigned char hist[3];
            sampleBgr(prevFrame, frameWidth, frameHeight, sourceX, srcY, hist);
            for (int c = 0; c < 3; ++c) {
                pixel[c] = static_cast<unsigned char>(__float2int_rn(
                    (1.0f - hole * 0.65f) * pixel[c] + (hole * 0.65f) * hist[c]));
            }
        }
        const size_t outputOffset = pixelIndex * 3;
        output[outputOffset] = pixel[0];
        output[outputOffset + 1] = pixel[1];
        output[outputOffset + 2] = pixel[2];
        return;
    }

    const bool halfSbs = mode == static_cast<int>(OutputMode::HalfSbs);
    const int eyeWidth = halfSbs ? max(1, frameWidth / 2) : frameWidth;
    const bool isFirstHalf = outX < eyeWidth;
    const bool leftEye = !((isFirstHalf) ^ swapEyes);
    const int localX = isFirstHalf ? outX : outX - eyeWidth;
    const float targetX = halfSbs
        ? (static_cast<float>(localX) + 0.5f) * static_cast<float>(frameWidth) /
              static_cast<float>(eyeWidth) - 0.5f
        : static_cast<float>(localX);
    float hole = 0.0f;
    const float sourceX = warpSourceX(targetX, y, leftEye, frame, rawDepth, minMax,
                                      depthWidth, depthHeight, frameWidth, frameHeight,
                                      focusDepth, maxDisparity, iterations, protectSubtitles,
                                      enableRgbDetail, detailStrength, &hole);
    unsigned char pixel[3];
    sampleBgr(frame, frameWidth, frameHeight, sourceX, y, pixel);
    if (hasHistory && prevFrame != nullptr && hole > 0.05f) {
        unsigned char hist[3];
        sampleBgr(prevFrame, frameWidth, frameHeight, sourceX, y, hist);
        for (int c = 0; c < 3; ++c) {
            pixel[c] = static_cast<unsigned char>(__float2int_rn(
                (1.0f - hole * 0.65f) * pixel[c] + (hole * 0.65f) * hist[c]));
        }
    }
    const size_t outputOffset = pixelIndex * 3;
    output[outputOffset] = pixel[0];
    output[outputOffset + 1] = pixel[1];
    output[outputOffset + 2] = pixel[2];
}

} // namespace

cv::Mat renderStereoCuda(const cv::Mat& bgrFrame,
                        const cv::Mat& rawDepth,
                        const DibrParameters& parameters,
                        const OutputMode mode) {
    if (bgrFrame.empty() || bgrFrame.type() != CV_8UC3) {
        throw std::invalid_argument("CUDA rendering input must be non-empty 8-bit BGR image.");
    }
    if (rawDepth.empty() || rawDepth.type() != CV_32FC1) {
        throw std::invalid_argument("CUDA rendering depth must be non-empty CV_32FC1 matrix.");
    }
    if (!std::isfinite(parameters.maxDisparity) || parameters.maxDisparity < 0.0f ||
        !std::isfinite(parameters.focusDepth) || parameters.focusDepth < 0.0f ||
        parameters.focusDepth > 1.0f || parameters.inverseIterations < 1) {
        throw std::invalid_argument("Invalid DIBR parameters.");
    }

    const cv::Mat frame = bgrFrame.isContinuous() ? bgrFrame : bgrFrame.clone();
    const cv::Mat depth = rawDepth.isContinuous() ? rawDepth : rawDepth.clone();
    const int width = frame.cols;
    const int height = frame.rows;
    const int depthWidth = depth.cols;
    const int depthHeight = depth.rows;
    const size_t frameBytes = frame.total() * frame.elemSize();
    const size_t depthValues = depth.total();
    const size_t partialValues = std::max<size_t>(
        1, (depthValues + kThreadsPerBlock - 1) / kThreadsPerBlock);
    const int eyeWidth = std::max(1, width / 2);
    const int outputWidth = (mode == OutputMode::Anaglyph || mode == OutputMode::DepthMap ||
                             mode == OutputMode::TopAndBottom)
        ? width
        : 2 * (mode == OutputMode::HalfSbs ? eyeWidth : width);
    const int outputHeight = height;
    const size_t outputBytes = static_cast<size_t>(outputWidth) * outputHeight * 3;

    DeviceScratch& scratch = g_cudaScratch;
    scratch.reserve(frameBytes, depthValues, partialValues, outputBytes);
    checkCuda(cudaMemcpy(scratch.frame, frame.data, frameBytes, cudaMemcpyHostToDevice),
              "Copy video frame to CUDA device");
    checkCuda(cudaMemcpy(scratch.rawDepth, depth.ptr<float>(), depthValues * sizeof(float),
                         cudaMemcpyHostToDevice),
              "Copy depth map to CUDA device");

    const int reductionBlocks = static_cast<int>(partialValues);
    reduceMinMaxKernel<<<reductionBlocks, kThreadsPerBlock,
                         2 * kThreadsPerBlock * sizeof(float)>>>(
        scratch.rawDepth, depthValues, scratch.partialMin, scratch.partialMax);
    checkCuda(cudaGetLastError(), "Launch CUDA depth reduction");
    finishMinMaxKernel<<<1, kThreadsPerBlock, 2 * kThreadsPerBlock * sizeof(float)>>>(
        scratch.partialMin, scratch.partialMax, partialValues, scratch.minMax);
    checkCuda(cudaGetLastError(), "Launch CUDA depth summary");

    // 1. Anti-flicker temporal range smoothing
    const float alpha = parameters.enableTemporalSmoothing ? parameters.temporalAlpha : 0.0f;
    applyTemporalMinMaxKernel<<<1, 1>>>(
        scratch.minMax, scratch.smoothedMinMax, alpha, scratch.hasHistory);
    checkCuda(cudaGetLastError(), "Launch temporal minMax smoothing");

    // 2. Anti-jitter temporal depth map blending
    const int depthBlocks = static_cast<int>((depthValues + kThreadsPerBlock - 1) / kThreadsPerBlock);
    const float blendFactor = parameters.enableTemporalSmoothing ? 0.30f : 0.0f;
    blendDepthKernel<<<depthBlocks, kThreadsPerBlock>>>(
        scratch.rawDepth, scratch.prevDepth, depthValues, blendFactor, scratch.hasHistory);
    checkCuda(cudaGetLastError(), "Launch temporal depth blending");
    scratch.hasHistory = true;

    const size_t outputPixels = static_cast<size_t>(outputWidth) * outputHeight;
    const int renderBlocks = static_cast<int>(
        (outputPixels + kThreadsPerBlock - 1) / kThreadsPerBlock);
    renderKernel<<<renderBlocks, kThreadsPerBlock>>>(
        scratch.frame,
        scratch.prevFrame,
        scratch.hasHistory,
        scratch.rawDepth, scratch.smoothedMinMax, scratch.output,
        width, height, depthWidth, depthHeight, outputWidth, outputHeight,
        static_cast<int>(mode), parameters.focusDepth, parameters.maxDisparity,
        std::clamp(parameters.inverseIterations, 1, 8),
        parameters.protectSubtitles,
        parameters.enableRgbDetail,
        parameters.detailStrength,
        parameters.swapEyes);
    checkCuda(cudaGetLastError(), "Launch CUDA DIBR render");

    // 保存当前帧到 prevFrame，供下一帧时序修补 (Temporal Background Inpainting)
    checkCuda(cudaMemcpy(scratch.prevFrame, scratch.frame, frameBytes, cudaMemcpyDeviceToDevice),
              "Cache current frame for temporal inpainting");
    scratch.hasHistory = true;

    cv::Mat rendered(outputHeight, outputWidth, CV_8UC3);
    checkCuda(cudaMemcpy(rendered.data, scratch.output, outputBytes, cudaMemcpyDeviceToHost),
              "Copy CUDA render result to host");
    return rendered;
}

void resetDibrTemporalState() {
    g_cudaScratch.resetHistory();
}

} // namespace parallaxrt
