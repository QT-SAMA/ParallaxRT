#pragma once

#include "core/depth_estimator.h"
#include "core/dibr_renderer.h"
#include "core/filter_config.h"
#include "core/tray_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <initguid.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace parallaxrt {

// {B413F85C-1065-42EB-8FF8-96E875150001}
DEFINE_GUID(CLSID_ParallaxRTTransformFilter,
0xb413f85c, 0x1065, 0x42eb, 0x8f, 0xf8, 0x96, 0xe8, 0x75, 0x15, 0x0, 0x1);

// {B413F85C-1065-42EB-8FF8-96E875150002}
DEFINE_GUID(IID_IParallaxRTTransformFilter,
0xb413f85c, 0x1065, 0x42eb, 0x8f, 0xf8, 0x96, 0xe8, 0x75, 0x15, 0x0, 0x2);

// Standard DirectShow YUV Subtypes
// NV12: 3231564E-0000-0010-8000-00AA00389B71
DEFINE_GUID(MEDIASUBTYPE_NV12_CUSTOM,
0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// YV12: 32315659-0000-0010-8000-00AA00389B71
DEFINE_GUID(MEDIASUBTYPE_YV12_CUSTOM,
0x32315659, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// YUY2: 32595559-0000-0010-8000-00AA00389B71
DEFINE_GUID(MEDIASUBTYPE_YUY2_CUSTOM,
0x32595559, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

class ParallaxRTTransformFilter;

// Input pin
class ParallaxRTInputPin : public IPin, public IMemInputPin {
public:
    ParallaxRTInputPin(ParallaxRTTransformFilter* filter);
    virtual ~ParallaxRTInputPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** ppAllocator) override;
    STDMETHODIMP NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly) override;
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps) override;
    STDMETHODIMP Receive(IMediaSample* pSample) override;
    STDMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed) override;
    STDMETHODIMP ReceiveCanBlock() override;

    IPin* connectedPin() const { return connectedPin_; }
    const AM_MEDIA_TYPE& currentMediaType() const { return mediaType_; }
    void updateMediaType(const AM_MEDIA_TYPE* pmt);

private:
    ParallaxRTTransformFilter* filter_;
    std::atomic_long refCount_{1};
    IPin* connectedPin_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE mediaType_{};
};

// Output pin
class ParallaxRTOutputPin : public IPin {
public:
    ParallaxRTOutputPin(ParallaxRTTransformFilter* filter);
    virtual ~ParallaxRTOutputPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    HRESULT deliver(IMediaSample* pSample);
    HRESULT decideAllocator(IMemInputPin* pPin, IMemAllocator** ppAlloc);

    IPin* connectedPin() const { return connectedPin_; }
    IMemInputPin* inputPin() const { return inputPin_; }
    IMemAllocator* allocator() const { return allocator_; }
    const AM_MEDIA_TYPE& currentMediaType() const { return mediaType_; }

private:
    ParallaxRTTransformFilter* filter_;
    std::atomic_long refCount_{1};
    IPin* connectedPin_ = nullptr;
    IMemInputPin* inputPin_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE mediaType_{};
};

// Main Filter
class ParallaxRTTransformFilter : public IBaseFilter {
public:
    ParallaxRTTransformFilter();
    virtual ~ParallaxRTTransformFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // Transform processing
    HRESULT processSample(IMediaSample* pSample);
    HRESULT checkInputType(const AM_MEDIA_TYPE* pmt);
    HRESULT checkTransform(const AM_MEDIA_TYPE* mtIn, const AM_MEDIA_TYPE* mtOut);
    HRESULT getMediaType(int iPosition, AM_MEDIA_TYPE* pmt);

    ParallaxRTInputPin* inputPin() { return inputPin_; }
    ParallaxRTOutputPin* outputPin() { return outputPin_; }

private:
    void ensureAIModelLoaded(int modelSelection, int aiResolution);
    std::filesystem::path locateDepthModel(const wchar_t* preferredFileName);
    void updateSerialSync(bool enabled, int comPort);
    void sendSerialSyncMarker(uint8_t marker);
    void sendSerialPhase(int32_t offsetUs);

    std::atomic_long refCount_{1};
    FILTER_STATE state_ = State_Stopped;
    IReferenceClock* clock_ = nullptr;
    IFilterGraph* graph_ = nullptr;
    std::wstring filterName_ = L"ParallaxRT Real-time AI 3D Filter";

    ParallaxRTInputPin* inputPin_ = nullptr;
    ParallaxRTOutputPin* outputPin_ = nullptr;

    std::mutex processMutex_;
    std::unique_ptr<DepthEstimator> estimator_;
    int loadedModelSelection_ = -1;
    int loadedAiResolution_ = -1;
    int lastLoadedConfigEpoch_ = -1;
    std::chrono::steady_clock::time_point lastModelLoadTime_{};
    cv::Mat cachedRawDepth_;
    size_t frameCount_ = 0;

    std::thread aiThread_;
    std::mutex aiMutex_;
    std::condition_variable aiCv_;
    bool aiThreadRunning_ = false;
    cv::Mat aiInputFrame_;
    bool hasNewAiFrame_ = false;
    void aiInferenceLoop();

    std::chrono::steady_clock::time_point lastFpsTime_{};
    size_t fpsFrameCount_ = 0;
    double currentFps_ = 0.0;

    CRITICAL_SECTION filterLock_;

    HANDLE syncSerialPort_ = INVALID_HANDLE_VALUE;
    int currentComPort_ = -1;
    int lastSentPhaseUs_ = INT_MIN;
};

} // namespace parallaxrt
