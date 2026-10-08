#include "core/directshow_filter.h"
#include <new>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace parallaxrt {

static void FilterLog(const char* fmt, ...) {
    static std::mutex s_logMutex;
    std::lock_guard<std::mutex> lock(s_logMutex);

    static std::wstring s_logPath;
    if (s_logPath.empty()) {
        wchar_t szPath[MAX_PATH] = {0};
        // Try getting directory of the current module (ParallaxRTFilter.ax)
        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CLSID_ParallaxRTTransformFilter), &hModule);
        if (hModule && GetModuleFileNameW(hModule, szPath, MAX_PATH) > 0) {
            std::filesystem::path p(szPath);
            std::filesystem::path candidate = p.parent_path() / L"parallaxrt_filter.log";
            FILE* testFp = nullptr;
            if (_wfopen_s(&testFp, candidate.c_str(), L"a") == 0 && testFp) {
                fclose(testFp);
                s_logPath = candidate.wstring();
            }
        }
        if (s_logPath.empty()) {
            wchar_t tempPath[MAX_PATH] = {0};
            if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                s_logPath = (std::filesystem::path(tempPath) / L"parallaxrt_filter.log").wstring();
            } else {
                s_logPath = L"parallaxrt_filter.log";
            }
        }
    }

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, s_logPath.c_str(), L"a") == 0 && fp) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(fp, "[%02d:%02d:%02d.%03d][TID:%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
        va_list args;
        va_start(args, fmt);
        vfprintf(fp, fmt, args);
        va_end(args);
        fprintf(fp, "\n");
        fflush(fp);
        fclose(fp);
    }
}

// Enumerator for pins
class EnumPinsImpl : public IEnumPins {
public:
    EnumPinsImpl(IPin* pin1, IPin* pin2) : pin1_(pin1), pin2_(pin2) {
        if (pin1_) pin1_->AddRef();
        if (pin2_) pin2_->AddRef();
    }
    virtual ~EnumPinsImpl() {
        if (pin1_) pin1_->Release();
        if (pin2_) pin2_->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG u = --refCount_;
        if (u == 0) delete this;
        return u;
    }

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < cPins && index_ < 2) {
            IPin* p = (index_ == 0) ? pin1_ : pin2_;
            if (p) {
                ppPins[fetched] = p;
                p->AddRef();
                fetched++;
            }
            index_++;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cPins) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        index_ += cPins;
        return (index_ <= 2) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        auto* e = new EnumPinsImpl(pin1_, pin2_);
        e->index_ = index_;
        *ppEnum = e;
        return S_OK;
    }

private:
    std::atomic_long refCount_{1};
    IPin* pin1_ = nullptr;
    IPin* pin2_ = nullptr;
    ULONG index_ = 0;
};

// Helper struct and functions to safely parse and copy video formats (VideoInfo & VideoInfo2)
struct VideoFormatInfo {
    int width = 0;
    int height = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    int bpp = 0;
    REFERENCE_TIME avgTimePerFrame = 166666; // default 60fps
    DWORD compression = 0;
    bool isTopDown = false;
};

static bool extractVideoFormatInfo(const AM_MEDIA_TYPE& mt, VideoFormatInfo& info) {
    if (mt.majortype != MEDIATYPE_Video || !mt.pbFormat) {
        return false;
    }
    
    auto parseFormat = [&](auto* vih) {
        info.surfaceWidth = vih->bmiHeader.biWidth;
        info.surfaceHeight = abs(vih->bmiHeader.biHeight);
        
        info.width = info.surfaceWidth;
        info.height = info.surfaceHeight;
        
        if (vih->rcTarget.right > vih->rcTarget.left) {
            info.width = vih->rcTarget.right - vih->rcTarget.left;
        } else if (vih->rcSource.right > vih->rcSource.left) {
            info.width = vih->rcSource.right - vih->rcSource.left;
        }
        
        if (vih->rcTarget.bottom > vih->rcTarget.top) {
            info.height = vih->rcTarget.bottom - vih->rcTarget.top;
        } else if (vih->rcSource.bottom > vih->rcSource.top) {
            info.height = vih->rcSource.bottom - vih->rcSource.top;
        }
        
        info.bpp = vih->bmiHeader.biBitCount;
        info.avgTimePerFrame = (vih->AvgTimePerFrame > 0) ? vih->AvgTimePerFrame : 166666;
        info.compression = vih->bmiHeader.biCompression;
        info.isTopDown = (vih->bmiHeader.biHeight < 0);
        return (info.width > 0 && info.height > 0);
    };

    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        return parseFormat(reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat));
    }
    if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        return parseFormat(reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat));
    }
    return false;
}

static void freeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat > 0 && mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
    memset(&mt, 0, sizeof(AM_MEDIA_TYPE));
}

static void freeAllocatedMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    freeMediaType(*mt);
    CoTaskMemFree(mt);
}

static bool calculateInputLayout(const AM_MEDIA_TYPE& mt,
                                 const VideoFormatInfo& info,
                                 size_t& rowStride,
                                 size_t& requiredBytes) {
    if (info.width <= 0 || info.height <= 0) return false;
    const size_t surfaceWidth = static_cast<size_t>(info.surfaceWidth > 0 ? info.surfaceWidth : info.width);
    const size_t surfaceHeight = static_cast<size_t>(info.surfaceHeight > 0 ? info.surfaceHeight : info.height);
    const size_t width = static_cast<size_t>(info.width);
    const size_t height = static_cast<size_t>(info.height);
    const DWORD fourcc = info.compression;

    if (mt.subtype == MEDIASUBTYPE_NV12_CUSTOM || fourcc == MAKEFOURCC('N','V','1','2') ||
        mt.subtype == MEDIASUBTYPE_YV12_CUSTOM || fourcc == MAKEFOURCC('Y','V','1','2')) {
        if ((info.width & 1) || (info.height & 1)) return false;
        rowStride = surfaceWidth;
        requiredBytes = surfaceWidth * surfaceHeight + (surfaceWidth * surfaceHeight) / 2;
        return true;
    }
    if (mt.subtype == MEDIASUBTYPE_YUY2_CUSTOM || fourcc == MAKEFOURCC('Y','U','Y','2')) {
        if (info.width & 1) return false;
        rowStride = surfaceWidth * 2;
        requiredBytes = rowStride * surfaceHeight;
        return true;
    }
    if (mt.subtype == MEDIASUBTYPE_RGB24 || info.bpp == 24) {
        rowStride = (surfaceWidth * 3 + 3) & ~static_cast<size_t>(3);
        requiredBytes = rowStride * surfaceHeight;
        return true;
    }
    if (mt.subtype == MEDIASUBTYPE_RGB32 || info.bpp == 32) {
        rowStride = surfaceWidth * 4;
        requiredBytes = rowStride * surfaceHeight;
        return true;
    }
    return false;
}

static bool inferUniform2xSizeMismatch(const AM_MEDIA_TYPE& mt,
                                       VideoFormatInfo& info,
                                       size_t availableBytes,
                                       size_t& rowStride,
                                       size_t& requiredBytes) {
    size_t currentStride = 0;
    size_t currentRequired = 0;
    if (!calculateInputLayout(mt, info, currentStride, currentRequired) || currentRequired == 0) {
        return false;
    }

    const double ratio = static_cast<double>(availableBytes) /
                         static_cast<double>(currentRequired);
    const int scale = (ratio >= 0.22 && ratio <= 0.30) ? 2
                    : (ratio >= 3.5 && ratio <= 4.5) ? -2 : 0;
    if (scale == 0) return false;

    VideoFormatInfo adjusted = info;
    if (scale == 2) {
        adjusted.width /= 2;
        adjusted.height /= 2;
    } else {
        if (adjusted.width > std::numeric_limits<int>::max() / 2 ||
            adjusted.height > std::numeric_limits<int>::max() / 2) return false;
        adjusted.width *= 2;
        adjusted.height *= 2;
    }

    size_t adjustedStride = 0;
    size_t adjustedRequired = 0;
    if (!calculateInputLayout(mt, adjusted, adjustedStride, adjustedRequired) ||
        availableBytes < adjustedRequired ||
        availableBytes > adjustedRequired + adjustedRequired / 5) {
        return false;
    }

    FilterLog("Correcting input media dimensions from %dx%d to %dx%d using sample length %zu (media type expected %zu)",
              info.width, info.height, adjusted.width, adjusted.height,
              availableBytes, currentRequired);
    info = adjusted;
    rowStride = adjustedStride;
    requiredBytes = adjustedRequired;
    return true;
}

static void refineInputStrideFromSampleLength(const AM_MEDIA_TYPE& mt,
                                              const VideoFormatInfo& info,
                                              size_t availableBytes,
                                              size_t& rowStride,
                                              size_t& requiredBytes) {
    if (info.height <= 0 || availableBytes == 0) return;

    const DWORD fourcc = info.compression;
    size_t rowCount = 0;
    size_t minimumStride = rowStride;
    if (mt.subtype == MEDIASUBTYPE_NV12_CUSTOM || fourcc == MAKEFOURCC('N','V','1','2') ||
        mt.subtype == MEDIASUBTYPE_YV12_CUSTOM || fourcc == MAKEFOURCC('Y','V','1','2')) {
        rowCount = static_cast<size_t>(info.height) + static_cast<size_t>(info.height / 2);
    } else if (mt.subtype == MEDIASUBTYPE_YUY2_CUSTOM || fourcc == MAKEFOURCC('Y','U','Y','2')) {
        rowCount = static_cast<size_t>(info.height);
    } else if (mt.subtype == MEDIASUBTYPE_RGB24 || info.bpp == 24 ||
               mt.subtype == MEDIASUBTYPE_RGB32 || info.bpp == 32) {
        rowCount = static_cast<size_t>(info.height);
    }
    if (rowCount == 0 || availableBytes % rowCount != 0) return;

    const size_t candidateStride = availableBytes / rowCount;
    if (candidateStride < minimumStride || candidateStride > minimumStride * 2) return;

    if ((mt.subtype == MEDIASUBTYPE_RGB32 || info.bpp == 32) && (candidateStride % 4 != 0)) return;
    if ((mt.subtype == MEDIASUBTYPE_RGB24 || info.bpp == 24) && (candidateStride % 4 != 0)) return;
    if ((mt.subtype == MEDIASUBTYPE_YUY2_CUSTOM || fourcc == MAKEFOURCC('Y','U','Y','2')) && (candidateStride % 2 != 0)) return;

    const size_t candidateBytes = candidateStride * rowCount;
    if (candidateBytes < requiredBytes) return;
    if (candidateStride != rowStride) {
        FilterLog("Using padded input stride %zu instead of %zu for %dx%d subtype=%08X (sample=%zu)",
                  candidateStride, rowStride, info.width, info.height,
                  mt.subtype.Data1, availableBytes);
        rowStride = candidateStride;
        requiredBytes = candidateBytes;
    }
}

static HRESULT copyMediaType(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    dst = src;
    if (src.cbFormat > 0 && src.pbFormat) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) return E_OUTOFMEMORY;
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    }
    if (dst.pUnk) {
        dst.pUnk->AddRef();
    }
    return S_OK;
}

// Enumerator for Media Types
class EnumMediaTypesImpl : public IEnumMediaTypes {
public:
    EnumMediaTypesImpl(const std::vector<AM_MEDIA_TYPE>& types) {
        types_.resize(types.size());
        for (size_t i = 0; i < types.size(); ++i) {
            types_[i] = types[i];
            if (types[i].cbFormat > 0 && types[i].pbFormat) {
                types_[i].pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(types[i].cbFormat));
                if (types_[i].pbFormat) {
                    memcpy(types_[i].pbFormat, types[i].pbFormat, types[i].cbFormat);
                }
            }
        }
    }
    virtual ~EnumMediaTypesImpl() {
        for (auto& mt : types_) {
            if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
            mt.pbFormat = nullptr;
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG u = --refCount_;
        if (u == 0) delete this;
        return u;
    }

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < cMediaTypes && index_ < types_.size()) {
            ppMediaTypes[fetched] = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!ppMediaTypes[fetched]) break;
            *ppMediaTypes[fetched] = types_[index_];
            if (types_[index_].cbFormat > 0 && types_[index_].pbFormat) {
                ppMediaTypes[fetched]->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(types_[index_].cbFormat));
                if (ppMediaTypes[fetched]->pbFormat) {
                    memcpy(ppMediaTypes[fetched]->pbFormat, types_[index_].pbFormat, types_[index_].cbFormat);
                }
            }
            fetched++;
            index_++;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        index_ += cMediaTypes;
        return (index_ <= types_.size()) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        auto* e = new EnumMediaTypesImpl(types_);
        e->index_ = index_;
        *ppEnum = e;
        return S_OK;
    }

private:
    std::atomic_long refCount_{1};
    std::vector<AM_MEDIA_TYPE> types_;
    ULONG index_ = 0;
};

// -------------------------------------------------------------
// ParallaxRTInputPin implementation
// -------------------------------------------------------------

ParallaxRTInputPin::ParallaxRTInputPin(ParallaxRTTransformFilter* filter) : filter_(filter) {
    memset(&mediaType_, 0, sizeof(mediaType_));
}

ParallaxRTInputPin::~ParallaxRTInputPin() {
    Disconnect();
}

STDMETHODIMP ParallaxRTInputPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IMemInputPin) {
        *ppv = static_cast<IMemInputPin*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ParallaxRTInputPin::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) ParallaxRTInputPin::Release() {
    ULONG u = --refCount_;
    if (u == 0) delete this;
    return u;
}

STDMETHODIMP ParallaxRTInputPin::Connect(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED; // input pins don't initiate Connect
}

STDMETHODIMP ParallaxRTInputPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    if (!pConnector || !pmt) return E_POINTER;
    if (connectedPin_ && connectedPin_ != pConnector) return VFW_E_ALREADY_CONNECTED;
    if (QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;

    if (!connectedPin_) {
        connectedPin_ = pConnector;
        connectedPin_->AddRef();
    }

    if (mediaType_.pbFormat) {
        CoTaskMemFree(mediaType_.pbFormat);
        mediaType_.pbFormat = nullptr;
    }
    copyMediaType(mediaType_, *pmt);

    VideoFormatInfo vfi;
    extractVideoFormatInfo(*pmt, vfi);
    FilterLog("InputPin::ReceiveConnection: subtype=%08X, res=%dx%d, bpp=%d, fourcc=0x%08X",
              pmt->subtype.Data1, vfi.width, vfi.height, vfi.bpp, vfi.compression);

    return S_OK;
}

void ParallaxRTInputPin::updateMediaType(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return;
    if (mediaType_.pbFormat) {
        CoTaskMemFree(mediaType_.pbFormat);
        mediaType_.pbFormat = nullptr;
    }
    copyMediaType(mediaType_, *pmt);
    VideoFormatInfo vfi;
    extractVideoFormatInfo(*pmt, vfi);
    FilterLog("InputPin::updateMediaType: updated res=%dx%d", vfi.width, vfi.height);
}

STDMETHODIMP ParallaxRTInputPin::Disconnect() {
    if (allocator_) {
        allocator_->Decommit();
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (connectedPin_) {
        connectedPin_->Release();
        connectedPin_ = nullptr;
    }
    if (mediaType_.pbFormat) {
        CoTaskMemFree(mediaType_.pbFormat);
        mediaType_.pbFormat = nullptr;
    }
    memset(&mediaType_, 0, sizeof(mediaType_));
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    if (!connectedPin_) return VFW_E_NOT_CONNECTED;
    *pPin = connectedPin_;
    connectedPin_->AddRef();
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!connectedPin_) return VFW_E_NOT_CONNECTED;
    *pmt = mediaType_;
    if (mediaType_.cbFormat > 0 && mediaType_.pbFormat) {
        pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mediaType_.cbFormat));
        memcpy(pmt->pbFormat, mediaType_.pbFormat, mediaType_.cbFormat);
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = filter_;
    if (filter_) filter_->AddRef();
    pInfo->dir = PINDIR_INPUT;
    wcscpy_s(pInfo->achName, L"Input");
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_INPUT;
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"In")));
    wcscpy_s(*Id, 3, L"In");
    return S_OK;
}

static void initMediaTypeVideoInfo(AM_MEDIA_TYPE& mt, const GUID& subtype, int bpp, DWORD compression, int width = 1920, int height = 1080) {
    memset(&mt, 0, sizeof(AM_MEDIA_TYPE));
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = subtype;
    mt.formattype = FORMAT_VideoInfo;
    mt.bFixedSizeSamples = TRUE;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!mt.pbFormat) return;
    memset(mt.pbFormat, 0, sizeof(VIDEOINFOHEADER));
    auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt.pbFormat);
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = width;
    vih->bmiHeader.biHeight = height;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = static_cast<WORD>(bpp);
    vih->bmiHeader.biCompression = compression;
    vih->bmiHeader.biSizeImage = (compression == BI_RGB)
        ? (width * height * (bpp / 8))
        : (width * height * 3 / 2);
    vih->AvgTimePerFrame = 166666;
    mt.lSampleSize = vih->bmiHeader.biSizeImage;
}

STDMETHODIMP ParallaxRTInputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype == MEDIATYPE_Video) {
        if (pmt->subtype == MEDIASUBTYPE_RGB24 || pmt->subtype == MEDIASUBTYPE_RGB32 ||
            pmt->subtype == MEDIASUBTYPE_NV12_CUSTOM || pmt->subtype == MEDIASUBTYPE_YV12_CUSTOM ||
            pmt->subtype == MEDIASUBTYPE_YUY2_CUSTOM) {
            return S_OK;
        }
        VideoFormatInfo info;
        if (extractVideoFormatInfo(*pmt, info)) {
            if (info.compression == MAKEFOURCC('N','V','1','2') ||
                info.compression == MAKEFOURCC('Y','V','1','2') ||
                info.compression == MAKEFOURCC('Y','U','Y','2') ||
                info.compression == BI_RGB) {
                return S_OK;
            }
        }
    }
    return S_FALSE;
}

STDMETHODIMP ParallaxRTInputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> types;
    if (connectedPin_ && mediaType_.majortype == MEDIATYPE_Video) {
        types.push_back(mediaType_);
    }
    *ppEnum = new EnumMediaTypesImpl(types);
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::QueryInternalConnections(IPin**, ULONG* nPin) {
    if (!nPin) return E_POINTER;
    return E_NOTIMPL; // 1:1 transform filter standard
}

STDMETHODIMP ParallaxRTInputPin::EndOfStream() {
    if (filter_->outputPin()->connectedPin()) {
        return filter_->outputPin()->connectedPin()->EndOfStream();
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::BeginFlush() {
    if (filter_->outputPin()->connectedPin()) {
        return filter_->outputPin()->connectedPin()->BeginFlush();
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::EndFlush() {
    if (filter_->outputPin()->connectedPin()) {
        return filter_->outputPin()->connectedPin()->EndFlush();
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) {
    if (filter_->outputPin()->connectedPin()) {
        return filter_->outputPin()->connectedPin()->NewSegment(tStart, tStop, dRate);
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::GetAllocator(IMemAllocator** ppAllocator) {
    if (!ppAllocator) return E_POINTER;
    if (!allocator_) {
        HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IMemAllocator, reinterpret_cast<void**>(&allocator_));
        if (FAILED(hr)) return hr;
    }
    *ppAllocator = allocator_;
    allocator_->AddRef();
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::NotifyAllocator(IMemAllocator* pAllocator, BOOL) {
    if (allocator_) {
        allocator_->Release();
    }
    allocator_ = pAllocator;
    if (allocator_) {
        allocator_->AddRef();
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) {
    return E_NOTIMPL;
}

STDMETHODIMP ParallaxRTInputPin::Receive(IMediaSample* pSample) {
    if (!pSample) return E_POINTER;
    return filter_->processSample(pSample);
}

STDMETHODIMP ParallaxRTInputPin::ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed) {
    if (!pSamples || !nSamplesProcessed) return E_POINTER;
    long processed = 0;
    for (long i = 0; i < nSamples; ++i) {
        if (FAILED(Receive(pSamples[i]))) break;
        processed++;
    }
    *nSamplesProcessed = processed;
    return S_OK;
}

STDMETHODIMP ParallaxRTInputPin::ReceiveCanBlock() {
    return S_OK;
}

// -------------------------------------------------------------
// ParallaxRTOutputPin implementation
// -------------------------------------------------------------

ParallaxRTOutputPin::ParallaxRTOutputPin(ParallaxRTTransformFilter* filter) : filter_(filter) {
    memset(&mediaType_, 0, sizeof(mediaType_));
}

ParallaxRTOutputPin::~ParallaxRTOutputPin() {
    Disconnect();
}

STDMETHODIMP ParallaxRTOutputPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ParallaxRTOutputPin::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) ParallaxRTOutputPin::Release() {
    ULONG u = --refCount_;
    if (u == 0) delete this;
    return u;
}

STDMETHODIMP ParallaxRTOutputPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (connectedPin_) return VFW_E_ALREADY_CONNECTED;

    VideoFormatInfo inputInfo;
    if (!extractVideoFormatInfo(filter_->inputPin()->currentMediaType(), inputInfo)) {
        FilterLog("OutputPin::Connect: refusing connection before a valid input media type is known");
        return VFW_E_NOT_CONNECTED;
    }

    if (pmt && QueryAccept(pmt) != S_OK) {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    bool connected = false;
    AM_MEDIA_TYPE negotiatedMt{};

    // A media type enumerated before the input pin connected may contain the
    // 1920x1080 fallback dimensions. Negotiate a fresh type using the connected
    // input dimensions instead of forwarding a potentially stale downstream type.
    const int preferredIndex = (pmt && pmt->subtype == MEDIASUBTYPE_RGB24) ? 1 : 0;
    for (int attempt = 0; attempt < 2 && !connected; ++attempt) {
        const int mediaTypeIndex = (preferredIndex + attempt) % 2;
        AM_MEDIA_TYPE candidateMt{};
        HRESULT hr = filter_->getMediaType(mediaTypeIndex, &candidateMt);
        if (FAILED(hr)) continue;

        VideoFormatInfo candidateInfo;
        if (!extractVideoFormatInfo(candidateMt, candidateInfo) ||
            candidateInfo.width != inputInfo.width ||
            candidateInfo.height != inputInfo.height) {
            freeMediaType(candidateMt);
            continue;
        }

        hr = pReceivePin->ReceiveConnection(this, &candidateMt);
        if (SUCCEEDED(hr)) {
            negotiatedMt = candidateMt;
            connected = true;
        } else {
            FilterLog("OutputPin::Connect: downstream rejected canonical RGB%d %dx%d, hr=0x%08X",
                      candidateInfo.bpp, candidateInfo.width, candidateInfo.height, hr);
            freeMediaType(candidateMt);
        }
    }

    if (!connected) {
        FilterLog("OutputPin::Connect: failed to negotiate acceptable media type with downstream pin");
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    connectedPin_ = pReceivePin;
    connectedPin_->AddRef();
    mediaType_ = negotiatedMt;

    HRESULT hr = connectedPin_->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&inputPin_));
    if (SUCCEEDED(hr) && inputPin_) {
        hr = decideAllocator(inputPin_, &allocator_);
    }

    VideoFormatInfo outputInfo;
    extractVideoFormatInfo(mediaType_, outputInfo);
    FilterLog("OutputPin::Connect: connected=1, output=%dx%d RGB%d sample=%lu, QI IMemInputPin hr=0x%08X, allocator=%p",
              outputInfo.width, outputInfo.height, outputInfo.bpp,
              static_cast<unsigned long>(mediaType_.lSampleSize), hr, allocator_);

    if (FAILED(hr)) {
        // Rollback on allocator failure
        pReceivePin->Disconnect();
        if (allocator_) {
            allocator_->Release();
            allocator_ = nullptr;
        }
        if (connectedPin_) {
            connectedPin_->Release();
            connectedPin_ = nullptr;
        }
        if (inputPin_) {
            inputPin_->Release();
            inputPin_ = nullptr;
        }
        freeMediaType(mediaType_);
        return hr;
    }

    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::decideAllocator(IMemInputPin* pPin, IMemAllocator** ppAlloc) {
    if (!pPin || !ppAlloc) return E_POINTER;

    ALLOCATOR_PROPERTIES prop{};
    prop.cBuffers = 6;
    prop.cbBuffer = mediaType_.lSampleSize > 0 ? mediaType_.lSampleSize : 1920 * 1080 * 4;
    prop.cbAlign = 1;
    prop.cbPrefix = 0;

    ALLOCATOR_PROPERTIES req{};
    if (SUCCEEDED(pPin->GetAllocatorRequirements(&req))) {
        if (req.cbBuffer > prop.cbBuffer) prop.cbBuffer = req.cbBuffer;
        if (req.cBuffers > prop.cBuffers) prop.cBuffers = req.cBuffers;
        if (req.cbAlign > prop.cbAlign) prop.cbAlign = req.cbAlign;
        if (req.cbPrefix > prop.cbPrefix) prop.cbPrefix = req.cbPrefix;
    }

    HRESULT hr = pPin->GetAllocator(ppAlloc);
    if (FAILED(hr) || !*ppAlloc) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMemAllocator, reinterpret_cast<void**>(ppAlloc));
        if (FAILED(hr)) return hr;
    }

    ALLOCATOR_PROPERTIES actual{};
    hr = (*ppAlloc)->SetProperties(&prop, &actual);
    if (FAILED(hr)) {
        (*ppAlloc)->Release();
        *ppAlloc = nullptr;
        return hr;
    }

    if (actual.cbBuffer < prop.cbBuffer) {
        FilterLog("OutputPin::decideAllocator: allocator returned %ld bytes; %ld required",
                  actual.cbBuffer, prop.cbBuffer);
        (*ppAlloc)->Release();
        *ppAlloc = nullptr;
        return VFW_E_BUFFER_OVERFLOW;
    }
    FilterLog("OutputPin::decideAllocator: requested=%ld actual=%ld buffers=%ld align=%ld",
              prop.cbBuffer, actual.cbBuffer, actual.cBuffers, actual.cbAlign);

    return pPin->NotifyAllocator(*ppAlloc, FALSE);
}

STDMETHODIMP ParallaxRTOutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED;
}

STDMETHODIMP ParallaxRTOutputPin::Disconnect() {
    if (allocator_) {
        allocator_->Decommit();
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (inputPin_) {
        inputPin_->Release();
        inputPin_ = nullptr;
    }
    if (connectedPin_) {
        connectedPin_->Release();
        connectedPin_ = nullptr;
    }
    if (mediaType_.pbFormat) {
        CoTaskMemFree(mediaType_.pbFormat);
        mediaType_.pbFormat = nullptr;
    }
    memset(&mediaType_, 0, sizeof(mediaType_));
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    if (!connectedPin_) return VFW_E_NOT_CONNECTED;
    *pPin = connectedPin_;
    connectedPin_->AddRef();
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (!connectedPin_) return VFW_E_NOT_CONNECTED;
    *pmt = mediaType_;
    if (mediaType_.cbFormat > 0 && mediaType_.pbFormat) {
        pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mediaType_.cbFormat));
        memcpy(pmt->pbFormat, mediaType_.pbFormat, mediaType_.cbFormat);
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = filter_;
    if (filter_) filter_->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Output");
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"Out")));
    wcscpy_s(*Id, 4, L"Out");
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype == MEDIATYPE_Video) {
        if (pmt->subtype == MEDIASUBTYPE_RGB24 || pmt->subtype == MEDIASUBTYPE_RGB32) {
            return S_OK;
        }
    }
    return S_FALSE;
}

STDMETHODIMP ParallaxRTOutputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> types(2);
    filter_->getMediaType(0, &types[0]);
    filter_->getMediaType(1, &types[1]);
    *ppEnum = new EnumMediaTypesImpl(types);
    for (auto& mt : types) {
        freeMediaType(mt);
    }
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::QueryInternalConnections(IPin**, ULONG* nPin) {
    if (!nPin) return E_POINTER;
    return E_NOTIMPL; // 1:1 transform filter standard
}

STDMETHODIMP ParallaxRTOutputPin::EndOfStream() {
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::BeginFlush() {
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::EndFlush() {
    return S_OK;
}

STDMETHODIMP ParallaxRTOutputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) {
    return S_OK;
}

HRESULT ParallaxRTOutputPin::deliver(IMediaSample* pSample) {
    if (!inputPin_) return VFW_E_NOT_CONNECTED;
    return inputPin_->Receive(pSample);
}

// -------------------------------------------------------------
// ParallaxRTTransformFilter implementation
// -------------------------------------------------------------

ParallaxRTTransformFilter::ParallaxRTTransformFilter() {
    FilterLog("ParallaxRTTransformFilter instance created");
    InitializeCriticalSection(&filterLock_);
    inputPin_ = new ParallaxRTInputPin(this);
    outputPin_ = new ParallaxRTOutputPin(this);
    aiThreadRunning_ = true;
    aiThread_ = std::thread(&ParallaxRTTransformFilter::aiInferenceLoop, this);
}

ParallaxRTTransformFilter::~ParallaxRTTransformFilter() {
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiThreadRunning_ = false;
        hasNewAiFrame_ = true; // Wake up the thread
    }
    aiCv_.notify_all();
    if (aiThread_.joinable()) {
        aiThread_.join();
    }
    
    if (inputPin_) {
        inputPin_->Release();
        inputPin_ = nullptr;
    }
    if (outputPin_) {
        outputPin_->Release();
        outputPin_ = nullptr;
    }
    if (syncSerialPort_ != INVALID_HANDLE_VALUE) {
        CloseHandle(syncSerialPort_);
        syncSerialPort_ = INVALID_HANDLE_VALUE;
        currentComPort_ = -1;
    }
    DeleteCriticalSection(&filterLock_);
}

void ParallaxRTTransformFilter::aiInferenceLoop() {
    FilterLog("AI Inference Thread started");
    while (true) {
        cv::Mat inputFrame;
        {
            std::unique_lock<std::mutex> lock(aiMutex_);
            aiCv_.wait(lock, [this]() { return hasNewAiFrame_ || !aiThreadRunning_; });
            if (!aiThreadRunning_) break;
            
            aiInputFrame_.copyTo(inputFrame);
            hasNewAiFrame_ = false;
        }

        if (inputFrame.empty()) continue;

        const auto cfg = FilterConfigManager::instance().getConfig();
        if (cfg.filterEnabled && estimator_) {
            // Frame cadence speedup for aiResolution (0: Full 1:1, 1: Balanced 1:2, 2: Fast 1:3)
            static size_t s_aiCadenceCounter = 0;
            s_aiCadenceCounter++;
            const int cadence = (cfg.aiResolution == 1) ? 2 : ((cfg.aiResolution == 2) ? 3 : 1);
            if (cadence > 1 && (s_aiCadenceCounter % cadence != 0)) {
                bool haveCachedDepth = false;
                {
                    std::lock_guard<std::mutex> lock(processMutex_);
                    haveCachedDepth = !cachedRawDepth_.empty();
                }
                if (haveCachedDepth) {
                    continue; // Reuse existing depth map, saving GPU compute and boosting FPS
                }
            }

            try {
                cv::Mat rawDepth = estimator_->infer(inputFrame);
                if (!rawDepth.empty()) {
                    std::lock_guard<std::mutex> lock(processMutex_);
                    cachedRawDepth_ = rawDepth;
                }
            } catch (const std::exception& ex) {
                FilterLog("AI Inference exception: %s", ex.what());
            }
        }
    }
    FilterLog("AI Inference Thread stopped");
}

STDMETHODIMP ParallaxRTTransformFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IParallaxRTTransformFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ParallaxRTTransformFilter::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) ParallaxRTTransformFilter::Release() {
    ULONG u = --refCount_;
    if (u == 0) delete this;
    return u;
}

STDMETHODIMP ParallaxRTTransformFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_ParallaxRTTransformFilter;
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::Stop() {
    EnterCriticalSection(&filterLock_);
    state_ = State_Stopped;
    if (outputPin_->allocator()) {
        outputPin_->allocator()->Decommit();
    }
    if (syncSerialPort_ != INVALID_HANDLE_VALUE) {
        CloseHandle(syncSerialPort_);
        syncSerialPort_ = INVALID_HANDLE_VALUE;
        currentComPort_ = -1;
    }
    parallaxrt::resetDibrTemporalState();
    cachedRawDepth_.release();
    frameCount_ = 0;
    lastSentPhaseUs_ = INT_MIN; 
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::Pause() {
    EnterCriticalSection(&filterLock_);
    state_ = State_Paused;
    if (outputPin_->allocator()) {
        outputPin_->allocator()->Commit();
    }
    // Launch tray icon & controller as soon as media graph loads/prerolls
    TrayController::instance().start();
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::Run(REFERENCE_TIME) {
    EnterCriticalSection(&filterLock_);
    state_ = State_Running;
    if (outputPin_->allocator()) {
        outputPin_->allocator()->Commit();
    }
    // Launch tray icon & controller on video playback start
    TrayController::instance().start();
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    *State = state_;
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::SetSyncSource(IReferenceClock* pClock) {
    EnterCriticalSection(&filterLock_);
    if (clock_) clock_->Release();
    clock_ = pClock;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    EnterCriticalSection(&filterLock_);
    *pClock = clock_;
    if (clock_) clock_->AddRef();
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new EnumPinsImpl(inputPin_, outputPin_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!Id || !ppPin) return E_POINTER;
    if (wcscmp(Id, L"In") == 0) {
        *ppPin = inputPin_;
        inputPin_->AddRef();
        return S_OK;
    }
    if (wcscmp(Id, L"Out") == 0) {
        *ppPin = outputPin_;
        outputPin_->AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP ParallaxRTTransformFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    wcscpy_s(pInfo->achName, filterName_.c_str());
    pInfo->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    FilterLog("ParallaxRTTransformFilter::JoinFilterGraph: pGraph=%p, name=%ls", pGraph, pName ? pName : L"null");
    EnterCriticalSection(&filterLock_);
    graph_ = pGraph;
    if (pName) filterName_ = pName;
    LeaveCriticalSection(&filterLock_);
    return S_OK;
}

STDMETHODIMP ParallaxRTTransformFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"ParallaxRT AI Team")));
    wcscpy_s(*pVendorInfo, 13, L"ParallaxRT AI Team");
    return S_OK;
}

std::filesystem::path ParallaxRTTransformFilter::locateDepthModel(const wchar_t* preferredFileName) {
    const std::filesystem::path modelName = preferredFileName;
    wchar_t szModule[MAX_PATH];
    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CLSID_ParallaxRTTransformFilter), &hModule);
    if (GetModuleFileNameW(hModule, szModule, MAX_PATH) > 0) {
        const auto dllDir = std::filesystem::path(szModule).parent_path();
        if (std::filesystem::is_regular_file(dllDir / modelName)) {
            FilterLog("Located model in dllDir: %ls", (dllDir / modelName).c_str());
            return dllDir / modelName;
        }
    }

    const std::filesystem::path standardPaths[] = {
        L"C:\\Program Files\\ParallaxRT",
        std::filesystem::current_path()
    };
    for (const auto& sp : standardPaths) {
        if (std::filesystem::is_regular_file(sp / modelName)) {
            FilterLog("Located model in standardPath: %ls", (sp / modelName).c_str());
            return sp / modelName;
        }
    }

    auto directory = std::filesystem::current_path();
    for (int level = 0; level < 6; ++level) {
        if (std::filesystem::is_regular_file(directory / modelName)) {
            FilterLog("Located model in current_path: %ls", (directory / modelName).c_str());
            return directory / modelName;
        }
        if (directory == directory.parent_path()) break;
        directory = directory.parent_path();
    }
    FilterLog("WARNING: Model not found on disk: %ls", modelName.c_str());
    return modelName;
}

void ParallaxRTTransformFilter::ensureAIModelLoaded(int modelSelection, int aiResolution) {
    if (estimator_ != nullptr && loadedModelSelection_ == modelSelection && loadedAiResolution_ == aiResolution) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (estimator_ != nullptr) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastModelLoadTime_).count();
        if (elapsedMs < 2000) {
            // Prevent reloading within 2 seconds to absorb UI jitter and rapid slider/combo changes
            return;
        }
    }
    lastModelLoadTime_ = now;

    const wchar_t* preferredFile = L"Depth Anything V2 (Base - ONNX).onnx";
    if (modelSelection == 1) {
        preferredFile = L"Depth Anything V2 (Small - ONNX).onnx";
    } else if (modelSelection == 2) {
        preferredFile = L"Depth Anything V2 (Large - ONNX).onnx";
    }

    auto modelPath = locateDepthModel(preferredFile);
    if (!std::filesystem::is_regular_file(modelPath)) {
        FilterLog("Preferred model '%ls' not found, searching fallbacks...", preferredFile);
        const wchar_t* fallbacks[] = {
            L"Depth Anything V2 (Base - ONNX).onnx",
            L"Depth Anything V2 (Small - ONNX).onnx",
            L"Depth Anything V2 (Large - ONNX).onnx"
        };
        for (const auto* fb : fallbacks) {
            auto candidate = locateDepthModel(fb);
            if (std::filesystem::is_regular_file(candidate)) {
                modelPath = candidate;
                FilterLog("Falling back to available model: %ls", candidate.c_str());
                break;
            }
        }
    }
    if (!std::filesystem::is_regular_file(modelPath)) {
        FilterLog("ERROR: Neither preferred nor fallback model file exists on disk!");
        return;
    }

    try {
        FilterLog("Initializing DepthEstimator with model: %ls...", modelPath.c_str());
        // Depth Anything V2 ONNX models contain a static 1370-token pos_embed (37x37 patches * 14 = 518x518).
        // The input tensor must strictly be 518x518 to avoid ONNX /Add broadcasting failure.
        constexpr int inputSize = 518;
        
        estimator_ = std::make_unique<DepthEstimator>(modelPath, defaultExecutionProvider(), inputSize);
        loadedModelSelection_ = modelSelection;
        loadedAiResolution_ = aiResolution;
        FilterLog("DepthEstimator loaded successfully!");
    } catch (const std::exception& ex) {
        FilterLog("EXCEPTION during DepthEstimator init: %s", ex.what());
    } catch (...) {
        FilterLog("UNKNOWN EXCEPTION during DepthEstimator init!");
    }
}

void ParallaxRTTransformFilter::updateSerialSync(bool enabled, int comPort) {
    if (!enabled) {
        if (syncSerialPort_ != INVALID_HANDLE_VALUE) {
            CloseHandle(syncSerialPort_);
            syncSerialPort_ = INVALID_HANDLE_VALUE;
            currentComPort_ = -1;
            FilterLog("SerialSync: Closed COM port");
        }
        return;
    }

    if (syncSerialPort_ != INVALID_HANDLE_VALUE && currentComPort_ == comPort) {
        return; // Already open on the desired port
    }

    if (syncSerialPort_ != INVALID_HANDLE_VALUE) {
        CloseHandle(syncSerialPort_);
        syncSerialPort_ = INVALID_HANDLE_VALUE;
        currentComPort_ = -1;
    }

    wchar_t portDeviceName[32];
    swprintf_s(portDeviceName, L"\\\\.\\COM%d", comPort);
    HANDLE hSerial = CreateFileW(
        portDeviceName,
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hSerial == INVALID_HANDLE_VALUE) {
        static int s_lastFailedPort = -1;
        if (s_lastFailedPort != comPort) {
            FilterLog("SerialSync: Failed to open %ls (Error: %lu)", portDeviceName, GetLastError());
            s_lastFailedPort = comPort;
        }
        currentComPort_ = comPort; // record so we don't spam open every frame
        return;
    }

    DCB dcbSerialParams{};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (GetCommState(hSerial, &dcbSerialParams)) {
        dcbSerialParams.BaudRate = CBR_115200;
        dcbSerialParams.ByteSize = 8;
        dcbSerialParams.StopBits = ONESTOPBIT;
        dcbSerialParams.Parity = NOPARITY;
        dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
        SetCommState(hSerial, &dcbSerialParams);
    }

    COMMTIMEOUTS timeouts{};
    timeouts.WriteTotalTimeoutConstant = 2; // Non-blocking: 2ms max write timeout
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(hSerial, &timeouts);

    syncSerialPort_ = hSerial;
    currentComPort_ = comPort;
    FilterLog("SerialSync: Successfully opened %ls at 115200 baud", portDeviceName);
}

void ParallaxRTTransformFilter::sendSerialSyncMarker(uint8_t marker) {
    if (syncSerialPort_ == INVALID_HANDLE_VALUE) return;
    const uint8_t packet[2] = { 0xAA, marker };
    DWORD bytesWritten = 0;
    WriteFile(syncSerialPort_, packet, sizeof(packet), &bytesWritten, nullptr);
}

void ParallaxRTTransformFilter::sendSerialPhase(int32_t offsetUs) {
    if (syncSerialPort_ == INVALID_HANDLE_VALUE) return;
    // 协议: 0xAA 0x03 <int16 big-endian>
    const uint16_t raw = static_cast<uint16_t>(static_cast<int16_t>(offsetUs));
    const uint8_t packet[4] = {
        0xAA, 0x03,
        static_cast<uint8_t>((raw >> 8) & 0xFF),
        static_cast<uint8_t>(raw & 0xFF)
    };
    DWORD bytesWritten = 0;
    WriteFile(syncSerialPort_, packet, sizeof(packet), &bytesWritten, nullptr);
}

HRESULT ParallaxRTTransformFilter::getMediaType(int iPosition, AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (iPosition < 0 || iPosition > 1) return VFW_S_NO_MORE_ITEMS;

    memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype = (iPosition == 0) ? MEDIASUBTYPE_RGB32 : MEDIASUBTYPE_RGB24;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!pmt->pbFormat) return E_OUTOFMEMORY;
    memset(pmt->pbFormat, 0, sizeof(VIDEOINFOHEADER));

    auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
    const int bpp = (iPosition == 0) ? 32 : 24;

    VideoFormatInfo info;
    if (extractVideoFormatInfo(inputPin_->currentMediaType(), info)) {
        vih->bmiHeader.biWidth = info.width;
        vih->bmiHeader.biHeight = -info.height; // OpenCV output buffers are top-down.
        vih->AvgTimePerFrame = info.avgTimePerFrame;
    } else {
        // Fallback default 1920x1080 for initial pin connection negotiation
        vih->bmiHeader.biWidth = 1920;
        vih->bmiHeader.biHeight = -1080;
        vih->AvgTimePerFrame = 166666; // ~60fps
    }

    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = static_cast<WORD>(bpp);
    vih->bmiHeader.biCompression = BI_RGB;
    const size_t rowStride =
        ((static_cast<size_t>(vih->bmiHeader.biWidth) * bpp + 31) / 32) * 4;
    const size_t imageSize = rowStride * static_cast<size_t>(abs(vih->bmiHeader.biHeight));
    if (imageSize > static_cast<size_t>(std::numeric_limits<LONG>::max())) {
        freeMediaType(*pmt);
        return VFW_E_BUFFER_OVERFLOW;
    }
    vih->rcSource = RECT{0, 0, vih->bmiHeader.biWidth, abs(vih->bmiHeader.biHeight)};
    vih->rcTarget = vih->rcSource;
    vih->bmiHeader.biSizeImage = static_cast<DWORD>(imageSize);
    pmt->lSampleSize = vih->bmiHeader.biSizeImage;

    return S_OK;
}

HRESULT ParallaxRTTransformFilter::processSample(IMediaSample* pSample) {
    if (!pSample) return E_POINTER;
    std::lock_guard<std::mutex> lock(processMutex_);

    BYTE* pSrc = nullptr;
    if (FAILED(pSample->GetPointer(&pSrc)) || !pSrc) {
        return S_OK;
    }

    AM_MEDIA_TYPE* attachedType = nullptr;
    const HRESULT attachedTypeHr = pSample->GetMediaType(&attachedType);
    std::unique_ptr<AM_MEDIA_TYPE, decltype(&freeAllocatedMediaType)> dynamicType(
        attachedType, &freeAllocatedMediaType);
    const AM_MEDIA_TYPE& inMt = (attachedTypeHr == S_OK && dynamicType)
        ? *dynamicType
        : inputPin_->currentMediaType();

    VideoFormatInfo info;
    if (!extractVideoFormatInfo(inMt, info) || info.width <= 0 || info.height <= 0) {
        FilterLog("processSample: invalid input media type (dynamic hr=0x%08X)", attachedTypeHr);
        return S_OK;
    }

    const long actualLength = pSample->GetActualDataLength();
    const long sampleCapacity = pSample->GetSize();
    const bool hasActualLength = actualLength > 0;
    const size_t availableBytes = static_cast<size_t>(
        hasActualLength ? actualLength : std::max(0L, sampleCapacity));
    size_t inputStride = 0;
    size_t requiredInputBytes = 0;
    if (!calculateInputLayout(inMt, info, inputStride, requiredInputBytes)) {
        FilterLog("processSample: unsupported or invalid input layout, res=%dx%d subtype=%08X bpp=%d fourcc=0x%08X",
                  info.width, info.height, inMt.subtype.Data1, info.bpp, info.compression);
        return S_OK;
    }

    const int width = info.width;
    const int height = info.height;
    const GUID& subtype = inMt.subtype;
    const DWORD fourcc = info.compression;

    if (hasActualLength) {
        inferUniform2xSizeMismatch(inMt, info, availableBytes, inputStride, requiredInputBytes);
        refineInputStrideFromSampleLength(inMt, info, availableBytes,
                                          inputStride, requiredInputBytes);
    }

    if (availableBytes < requiredInputBytes || sampleCapacity < 0 ||
        static_cast<size_t>(sampleCapacity) < requiredInputBytes) {
        FilterLog("processSample: input sample too small, media=%dx%d subtype=%08X actual=%ld capacity=%ld required=%zu",
                  info.width, info.height, inMt.subtype.Data1, actualLength,
                  sampleCapacity, requiredInputBytes);
        return S_OK;
    }

    if (frameCount_ == 0 || frameCount_ % 100 == 0) {
        VideoFormatInfo outInfo;
        const bool haveOutputInfo = extractVideoFormatInfo(outputPin_->currentMediaType(), outInfo);
        FilterLog("processSample frame %zu: input=%dx%d subtype=%08X actual=%ld capacity=%ld required=%zu output=%dx%d subtype=%08X sample=%lu dynamicType=%d",
                  frameCount_, width, height, subtype.Data1, actualLength, sampleCapacity,
                  requiredInputBytes,
                  haveOutputInfo ? outInfo.width : 0,
                  haveOutputInfo ? outInfo.height : 0,
                  outputPin_->currentMediaType().subtype.Data1,
                  static_cast<unsigned long>(outputPin_->currentMediaType().lSampleSize),
                  dynamicType ? 1 : 0);
    }

    cv::Mat inputFrame;
    try {
        if (subtype == MEDIASUBTYPE_NV12_CUSTOM || fourcc == MAKEFOURCC('N','V','1','2')) {
            cv::Mat nv12(height + height / 2, width, CV_8UC1, pSrc, inputStride);
            cv::cvtColor(nv12, inputFrame, cv::COLOR_YUV2BGR_NV12);
        } else if (subtype == MEDIASUBTYPE_YV12_CUSTOM || fourcc == MAKEFOURCC('Y','V','1','2')) {
            cv::Mat yv12(height + height / 2, width, CV_8UC1, pSrc, inputStride);
            cv::cvtColor(yv12, inputFrame, cv::COLOR_YUV2BGR_YV12);
        } else if (subtype == MEDIASUBTYPE_YUY2_CUSTOM || fourcc == MAKEFOURCC('Y','U','Y','2')) {
            cv::Mat yuy2(height, width, CV_8UC2, pSrc, inputStride);
            cv::cvtColor(yuy2, inputFrame, cv::COLOR_YUV2BGR_YUY2);
        } else if (subtype == MEDIASUBTYPE_RGB24 || info.bpp == 24) {
            inputFrame = cv::Mat(height, width, CV_8UC3, pSrc, inputStride).clone();
            if (!info.isTopDown) {
                cv::flip(inputFrame, inputFrame, 0);
            }
        } else if (subtype == MEDIASUBTYPE_RGB32 || info.bpp == 32) {
            cv::Mat bgra(height, width, CV_8UC4, pSrc, inputStride);
            cv::cvtColor(bgra, inputFrame, cv::COLOR_BGRA2BGR);
            if (!info.isTopDown) {
                cv::flip(inputFrame, inputFrame, 0);
            }
        }
    } catch (const cv::Exception& ex) {
        FilterLog("processSample: input pixel conversion failed for %dx%d subtype=%08X: %s",
                  width, height, subtype.Data1, ex.what());
        return S_OK;
    }

    if (inputFrame.empty() || inputFrame.cols != width || inputFrame.rows != height) {
        FilterLog("processSample: conversion produced invalid frame %dx%d for input %dx%d",
                  inputFrame.cols, inputFrame.rows, width, height);
        return S_OK;
    }

    const auto cfg = FilterConfigManager::instance().getConfig();
    cv::Mat rendered;

    if (!cfg.filterEnabled) {
        // Filter disabled: smooth 2D passthrough
        rendered = inputFrame;
    } else {
        const int curEpoch = FilterConfigManager::instance().getConfigEpoch();
        if (estimator_ == nullptr || lastLoadedConfigEpoch_ != curEpoch) {
            ensureAIModelLoaded(cfg.modelSelection, cfg.aiResolution);
            lastLoadedConfigEpoch_ = curEpoch;
        }
        if (!estimator_) {
            // Model loading or unavailable: smooth 2D passthrough
            rendered = inputFrame;
        } else {
            // Pass frame to async thread
            {
                std::lock_guard<std::mutex> aiLock(aiMutex_);
                inputFrame.copyTo(aiInputFrame_);
                hasNewAiFrame_ = true;
            }
            aiCv_.notify_one();

            cv::Mat rawDepth;
            if (!cachedRawDepth_.empty()) {
                rawDepth = cachedRawDepth_;
            }

            if (rawDepth.empty()) {
                // If depth isn't ready, pass through until it is
                rendered = inputFrame;
            } else {
                // DIBR Rendering parameters
                DibrParameters dibr;
                dibr.maxDisparity = static_cast<float>(cfg.maxDisparity);
                dibr.focusDepth = static_cast<float>(cfg.focusPercent) / 100.0f;
                dibr.protectSubtitles = cfg.protectSubtitles;
                dibr.enableTemporalSmoothing = cfg.antiFlicker;
                dibr.temporalAlpha = 0.85f;
                dibr.enableRgbDetail = cfg.enableRgbDetail;
                dibr.detailStrength = static_cast<float>(cfg.detailStrengthPercent) / 100.0f;
                dibr.swapEyes = cfg.swapEyes;

                OutputMode outMode = OutputMode::Anaglyph;
                if (cfg.mode == 1) outMode = OutputMode::HalfSbs;
                else if (cfg.mode == 2) outMode = OutputMode::FullSbs;
                else if (cfg.mode == 3) outMode = OutputMode::DepthMap;
                else if (cfg.mode == 4) outMode = OutputMode::TopAndBottom;

                try {
                    rendered = renderStereoFromRawDepth(inputFrame, rawDepth, dibr, outMode);
                } catch (const std::exception& ex) {
                    static bool s_loggedRenderError = false;
                    if (!s_loggedRenderError) {
                        FilterLog("renderStereoFromRawDepth exception: %s", ex.what());
                        s_loggedRenderError = true;
                    }
                    rendered = inputFrame;
                }
            }
        }
    }

    if (rendered.empty()) {
        FilterLog("processSample: renderer returned an empty frame");
        return S_OK;
    }

    const auto& outMt = outputPin_->currentMediaType();
    VideoFormatInfo outInfo;
    if (!extractVideoFormatInfo(outMt, outInfo) || outInfo.width <= 0 || outInfo.height <= 0 ||
        (outMt.subtype != MEDIASUBTYPE_RGB24 && outMt.subtype != MEDIASUBTYPE_RGB32)) {
        FilterLog("processSample: invalid negotiated output media type subtype=%08X",
                  outMt.subtype.Data1);
        return VFW_E_INVALIDMEDIATYPE;
    }
    const int targetWidth = outInfo.width;
    const int targetHeight = outInfo.height;

    // Helper to deliver a single subframe / frame
    auto deliverSubSample = [&](const cv::Mat& srcFrame, REFERENCE_TIME tSubStart, REFERENCE_TIME tSubStop, uint8_t serialMarker) -> HRESULT {
        cv::Mat frameToDeliver = srcFrame;
        if (frameToDeliver.size() != cv::Size(targetWidth, targetHeight)) {
            try {
                cv::resize(frameToDeliver, frameToDeliver, cv::Size(targetWidth, targetHeight),
                           0.0, 0.0, cv::INTER_LINEAR);
            } catch (const cv::Exception& ex) {
                FilterLog("deliverSubSample: resize failed: %s", ex.what());
                return S_OK;
            }
        }

        if (!outputPin_->allocator()) {
            FilterLog("deliverSubSample: outputPin_->allocator is null!");
            return S_OK;
        }
        IMediaSample* pOutSample = nullptr;
        HRESULT hrAlloc = outputPin_->allocator()->GetBuffer(&pOutSample, nullptr, nullptr, 0);
        if (hrAlloc == VFW_E_NOT_COMMITTED) {
            const HRESULT commitHr = outputPin_->allocator()->Commit();
            if (SUCCEEDED(commitHr) || commitHr == VFW_E_ALREADY_COMMITTED) {
                hrAlloc = outputPin_->allocator()->GetBuffer(&pOutSample, nullptr, nullptr, 0);
            }
        }
        if (FAILED(hrAlloc) || !pOutSample) {
            FilterLog("deliverSubSample: allocator GetBuffer failed: hr=0x%08X", hrAlloc);
            return S_OK;
        }

        BYTE* pDst = nullptr;
        HRESULT hrPtr = pOutSample->GetPointer(&pDst);
        const bool outIsRgb24 = (outMt.subtype == MEDIASUBTYPE_RGB24);
        const size_t bytesPerPixel = outIsRgb24 ? 3u : 4u;
        const size_t outSurfaceWidth = static_cast<size_t>(outInfo.surfaceWidth > 0 ? outInfo.surfaceWidth : outInfo.width);
        const size_t outSurfaceHeight = static_cast<size_t>(outInfo.surfaceHeight > 0 ? outInfo.surfaceHeight : outInfo.height);
        const size_t outputStride =
            ((outSurfaceWidth * bytesPerPixel + 3) / 4) * 4;
        const size_t requiredOutputBytes = outputStride * outSurfaceHeight;
        const long outputCapacity = pOutSample->GetSize();
        if (FAILED(hrPtr) || !pDst || outputCapacity < 0 ||
            static_cast<size_t>(outputCapacity) < requiredOutputBytes ||
            requiredOutputBytes > static_cast<size_t>(std::numeric_limits<LONG>::max())) {
            FilterLog("deliverSubSample: invalid output buffer capacity=%ld required=%zu hr=0x%08X",
                      outputCapacity, requiredOutputBytes, hrPtr);
            pOutSample->Release();
            return S_OK;
        }

        // Adapt to downstream allocator's pitch alignment (e.g. Direct3D11 Video Renderer 128-byte/64-byte row pitch)
        size_t effectiveOutputStride = outputStride;
        if (outSurfaceHeight > 0 && outputCapacity > 0) {
            const size_t cap = static_cast<size_t>(outputCapacity);
            if (cap >= requiredOutputBytes) {
                if (cap % outSurfaceHeight == 0) {
                    const size_t candidateStride = cap / outSurfaceHeight;
                    if (candidateStride >= outputStride && candidateStride <= outputStride * 2 && (candidateStride % 4 == 0)) {
                        effectiveOutputStride = candidateStride;
                    }
                } else {
                    for (size_t alignBytes : {128u, 64u, 256u}) {
                        const size_t alignedStride = ((outSurfaceWidth * bytesPerPixel + alignBytes - 1) / alignBytes) * alignBytes;
                        if (alignedStride >= outputStride && alignedStride * outSurfaceHeight <= cap) {
                            effectiveOutputStride = alignedStride;
                            break;
                        }
                    }
                }
            }
        }

        const size_t totalOutputBytes = effectiveOutputStride * outSurfaceHeight;

        try {
            cv::Mat outBgra;
            if (!outIsRgb24) {
                cv::cvtColor(frameToDeliver, outBgra, cv::COLOR_BGR2BGRA);
            }
            memset(pDst, 0, std::min(totalOutputBytes, static_cast<size_t>(outputCapacity)));
            for (int y = 0; y < targetHeight; ++y) {
                const int sourceY = outInfo.isTopDown ? y : (targetHeight - 1 - y);
                BYTE* destinationRow = pDst + static_cast<size_t>(y) * effectiveOutputStride;
                if (outIsRgb24) {
                    memcpy(destinationRow, frameToDeliver.ptr(sourceY),
                           static_cast<size_t>(targetWidth) * 3);
                } else {
                    memcpy(destinationRow, outBgra.ptr(sourceY),
                           static_cast<size_t>(targetWidth) * 4);
                }
            }
            pOutSample->SetActualDataLength(static_cast<LONG>(totalOutputBytes));
        } catch (const cv::Exception& ex) {
            FilterLog("deliverSubSample: output conversion failed: %s", ex.what());
            pOutSample->Release();
            return S_OK;
        }

        if (tSubStart >= 0 && tSubStop > tSubStart) {
            pOutSample->SetTime(&tSubStart, &tSubStop);
        }
        pOutSample->SetSyncPoint(pSample->IsSyncPoint() == S_OK);
        pOutSample->SetPreroll(pSample->IsPreroll() == S_OK);
        pOutSample->SetDiscontinuity(pSample->IsDiscontinuity() == S_OK);

        updateSerialSync(cfg.serialSyncEnabled, cfg.syncComPort);

        if (cfg.serialSyncEnabled && syncSerialPort_ != INVALID_HANDLE_VALUE &&
            cfg.phaseOffsetUs != lastSentPhaseUs_) {
            sendSerialPhase(cfg.phaseOffsetUs);
            lastSentPhaseUs_ = cfg.phaseOffsetUs;
        }

        HRESULT hrDeliver = outputPin_->deliver(pOutSample);
        pOutSample->Release();

        if (cfg.serialSyncEnabled && serialMarker != 0) {
            sendSerialSyncMarker(serialMarker);
        }
        return hrDeliver;
    };

    REFERENCE_TIME tStart = 0, tStop = 0;
    const bool hasTime = SUCCEEDED(pSample->GetTime(&tStart, &tStop));
    const uint8_t marker = cfg.serialSyncEnabled ? (((frameCount_ % 2 == 0) != cfg.swapEyes) ? 0x01 : 0x02) : 0x00;
    HRESULT hr = deliverSubSample(rendered, hasTime ? tStart : -1, hasTime ? tStop : -1, marker);
    fpsFrameCount_++;

    // Live FPS and Status Telemetry Calculation
    const auto now = std::chrono::steady_clock::now();
    if (lastFpsTime_.time_since_epoch().count() == 0) {
        lastFpsTime_ = now;
    } else {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime_).count();
        if (elapsedMs >= 500) {
            currentFps_ = (fpsFrameCount_ * 1000.0) / static_cast<double>(elapsedMs);
            fpsFrameCount_ = 0;
            lastFpsTime_ = now;
        }
    }

    FilterConfigManager::instance().updateTelemetry(
        currentFps_, width, height, targetWidth, targetHeight,
        /*isProcessing=*/true, /*aiActive=*/(cfg.filterEnabled && estimator_ != nullptr));

    frameCount_++;
    return hr;
}

} // namespace parallaxrt
