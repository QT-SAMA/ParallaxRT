#include "core/directshow_filter.h"
#include "core/tray_controller.h"

#include <windows.h>
#include <olectl.h>
#include <dshow.h>

static HINSTANCE g_hModule = nullptr;

class ParallaxRTClassFactory : public IClassFactory {
public:
    ParallaxRTClassFactory() = default;
    virtual ~ParallaxRTClassFactory() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;

        auto* filter = new (std::nothrow) parallaxrt::ParallaxRTTransformFilter();
        if (!filter) return E_OUTOFMEMORY;

        HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override {
        return S_OK;
    }

private:
    std::atomic_long refCount_{1};
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);

        wchar_t szModule[MAX_PATH];
        if (GetModuleFileNameW(hinstDLL, szModule, MAX_PATH) > 0) {
            wchar_t* lastSlash = wcsrchr(szModule, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                SetDllDirectoryW(szModule);
            }
        }
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        parallaxrt::TrayController::instance().stop();
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    if (rclsid == parallaxrt::CLSID_ParallaxRTTransformFilter) {
        auto* factory = new (std::nothrow) ParallaxRTClassFactory();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return S_OK;
}

// Media types for standard DirectShow Filter Mapper registration
static const REGPINTYPES sudPinTypesIn[] = {
    { &MEDIATYPE_Video, &parallaxrt::MEDIASUBTYPE_NV12_CUSTOM },
    { &MEDIATYPE_Video, &parallaxrt::MEDIASUBTYPE_YV12_CUSTOM },
    { &MEDIATYPE_Video, &parallaxrt::MEDIASUBTYPE_YUY2_CUSTOM },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 }
};

static const REGPINTYPES sudPinTypesOut[] = {
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 }
};

static const REGFILTERPINS sudPins[] = {
    {
        const_cast<LPWSTR>(L"Input"),
        FALSE,              // bRendered
        FALSE,              // bOutput
        FALSE,              // bZero
        FALSE,              // bMany
        nullptr,            // clsConnectsToFilter
        nullptr,            // strConnectsToPin
        5,                  // nMediaTypes
        sudPinTypesIn       // lpMediaType
    },
    {
        const_cast<LPWSTR>(L"Output"),
        FALSE,              // bRendered
        TRUE,               // bOutput
        FALSE,              // bZero
        FALSE,              // bMany
        nullptr,            // clsConnectsToFilter
        nullptr,            // strConnectsToPin
        2,                  // nMediaTypes
        sudPinTypesOut      // lpMediaType
    }
};

static const REGFILTER2 sudFilterReg = {
    1,                      // dwVersion
    0x00600000,             // dwMerit: MERIT_DO_NOT_USE + 0x400000 (standard transform merit)
    2,                      // cPins
    sudPins                 // rgPins
};

// Standard DllRegisterServer with IFilterMapper2
STDAPI DllRegisterServer() {
    wchar_t szModule[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, szModule, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Clean up any legacy manual category registry entry to eliminate duplicate entries in PotPlayer
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{083863F1-70DE-11d0-BD40-00A0C911CE86}\\Instance\\{B413F85C-1065-42EB-8FF8-96E875150001}");

    // Register CLSID in HKEY_CLASSES_ROOT
    const wchar_t szClsid[] = L"CLSID\\{B413F85C-1065-42EB-8FF8-96E875150001}";
    const wchar_t szInproc[] = L"CLSID\\{B413F85C-1065-42EB-8FF8-96E875150001}\\InprocServer32";

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, szClsid, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        const wchar_t desc[] = L"ParallaxRT Real-time 3D Filter";
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(desc), sizeof(desc));
        RegCloseKey(hKey);
    }

    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, szInproc, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(szModule), static_cast<DWORD>((wcslen(szModule) + 1) * sizeof(wchar_t)));
        const wchar_t threadModel[] = L"Both";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threadModel), sizeof(threadModel));
        RegCloseKey(hKey);
    }

    // Register filter with FilterMapper2 so DirectShow & PotPlayer know pins and media types
    HRESULT hrCo = CoInitialize(nullptr);
    IFilterMapper2* pFM = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void**>(&pFM));
    if (SUCCEEDED(hr) && pFM != nullptr) {
        hr = pFM->RegisterFilter(
            parallaxrt::CLSID_ParallaxRTTransformFilter,
            L"ParallaxRT Real-time 3D Filter",
            nullptr,
            &CLSID_LegacyAmFilterCategory,
            L"ParallaxRT Real-time 3D Filter",
            &sudFilterReg
        );
        pFM->Release();
    }
    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return hr;
}

STDAPI DllUnregisterServer() {
    HRESULT hrCo = CoInitialize(nullptr);
    IFilterMapper2* pFM = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void**>(&pFM));
    if (SUCCEEDED(hr) && pFM != nullptr) {
        pFM->UnregisterFilter(&CLSID_LegacyAmFilterCategory, L"ParallaxRT Real-time 3D Filter", parallaxrt::CLSID_ParallaxRTTransformFilter);
        pFM->Release();
    }
    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    // Delete both the FilterMapper2 category entry and any legacy category entry
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{083863F1-70DE-11d0-BD40-00A0C911CE86}\\Instance\\{B413F85C-1065-42EB-8FF8-96E875150001}");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{083863F1-70DE-11d0-BD40-00A0C911CE86}\\Instance\\ParallaxRT Real-time 3D Filter");

    const wchar_t szInproc[] = L"CLSID\\{B413F85C-1065-42EB-8FF8-96E875150001}\\InprocServer32";
    const wchar_t szClsid[] = L"CLSID\\{B413F85C-1065-42EB-8FF8-96E875150001}";

    RegDeleteKeyW(HKEY_CLASSES_ROOT, szInproc);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, szClsid);

    return S_OK;
}
