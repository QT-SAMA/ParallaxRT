#pragma once

#include <atomic>
#include <string>
#include <windows.h>

namespace parallaxrt {

struct FilterConfig {
    int mode = 0;              // 0: Anaglyph, 1: HalfSbs, 2: FullSbs, 3: DepthMap, 4: TopAndBottom
    int modelSelection = 0;    // 0: Base, 1: Small
    int maxDisparity = 24;     // 0 - 120 px
    int focusPercent = 50;     // 0 - 100%
    bool enableTemporalReuse = true; // 1:2
    bool protectSubtitles = true;
    bool antiFlicker = true;
    bool enableRgbDetail = true;
    int detailStrengthPercent = 22; // 0 - 60%
    bool filterEnabled = true; // master bypass toggle
    bool serialSyncEnabled = false; // Method A: ESP32 / micro-controller sync
    int syncComPort = 3;            // COM port number (e.g. 3 for COM3)
    bool swapEyes = false;          // Swap Left and Right eyes (左右眼交换/翻转)
    int language = 0;               // 0: English (default), 1: Simplified Chinese (简体中文)
    int aiResolution = 0;           // 0: 518x518 (Default), 1: 392x392 (Faster), 2: 252x252 (Fastest)
    int phaseOffsetUs = 0;     // NEW: DLP-Link pulse phase offset in us, range -4000..+4000
};

struct FilterTelemetry {
    double currentFps = 0.0;
    int inputWidth = 0;
    int inputHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    bool isProcessing = false;
    bool aiActive = false;
    ULONGLONG lastActiveTick = 0;
};

// Thread-safe settings management with Windows Registry persistence
class FilterConfigManager {
public:
    static FilterConfigManager& instance() {
        static FilterConfigManager inst;
        return inst;
    }

    FilterConfig getConfig() const {
        EnterCriticalSection(&cs_);
        FilterConfig copy = config_;
        LeaveCriticalSection(&cs_);
        return copy;
    }

    int getConfigEpoch() const {
        return configEpoch_.load(std::memory_order_relaxed);
    }

    void setConfig(const FilterConfig& cfg) {
        EnterCriticalSection(&cs_);
        config_ = cfg;
        LeaveCriticalSection(&cs_);
        configEpoch_.fetch_add(1, std::memory_order_relaxed);
        saveToRegistry();
    }

    void updateTelemetry(double fps, int inW, int inH, int outW, int outH, bool isProc, bool aiAct) {
        EnterCriticalSection(&cs_);
        telemetry_.currentFps = fps;
        telemetry_.inputWidth = inW;
        telemetry_.inputHeight = inH;
        telemetry_.outputWidth = outW;
        telemetry_.outputHeight = outH;
        telemetry_.isProcessing = isProc;
        telemetry_.aiActive = aiAct;
        telemetry_.lastActiveTick = GetTickCount64();
        LeaveCriticalSection(&cs_);
    }

    FilterTelemetry getTelemetry() const {
        EnterCriticalSection(&cs_);
        FilterTelemetry copy = telemetry_;
        LeaveCriticalSection(&cs_);
        return copy;
    }

    void resetToDefaults() {
        EnterCriticalSection(&cs_);
        const int preservedLang = config_.language;
        config_ = FilterConfig();
        config_.language = preservedLang; // retain user's current chosen language
        LeaveCriticalSection(&cs_);
        configEpoch_.fetch_add(1, std::memory_order_relaxed);
        saveToRegistry();
    }

    void loadFromRegistry() {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            readDword(hKey, L"Mode", config_.mode);
            readDword(hKey, L"ModelSelection", config_.modelSelection);
            readDword(hKey, L"MaxDisparity", config_.maxDisparity);
            readDword(hKey, L"FocusPercent", config_.focusPercent);
            readBool(hKey, L"TemporalReuse", config_.enableTemporalReuse);
            readBool(hKey, L"ProtectSubtitles", config_.protectSubtitles);
            readBool(hKey, L"AntiFlicker", config_.antiFlicker);
            readBool(hKey, L"EnableRgbDetail", config_.enableRgbDetail);
            readDword(hKey, L"DetailStrengthPercent", config_.detailStrengthPercent);
            readBool(hKey, L"FilterEnabled", config_.filterEnabled);
            readBool(hKey, L"SerialSyncEnabled", config_.serialSyncEnabled);
            readDword(hKey, L"SyncComPort", config_.syncComPort);
            readBool(hKey, L"SwapEyes", config_.swapEyes);
            readDword(hKey, L"Language", config_.language);
            readDword(hKey, L"AiResolution", config_.aiResolution);
            readDword(hKey, L"PhaseOffsetUs", config_.phaseOffsetUs);
            RegCloseKey(hKey);
        }
    }

    void saveToRegistry() const {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            writeDword(hKey, L"Mode", config_.mode);
            writeDword(hKey, L"ModelSelection", config_.modelSelection);
            writeDword(hKey, L"MaxDisparity", config_.maxDisparity);
            writeDword(hKey, L"FocusPercent", config_.focusPercent);
            writeBool(hKey, L"TemporalReuse", config_.enableTemporalReuse);
            writeBool(hKey, L"ProtectSubtitles", config_.protectSubtitles);
            writeBool(hKey, L"AntiFlicker", config_.antiFlicker);
            writeBool(hKey, L"EnableRgbDetail", config_.enableRgbDetail);
            writeDword(hKey, L"DetailStrengthPercent", config_.detailStrengthPercent);
            writeBool(hKey, L"FilterEnabled", config_.filterEnabled);
            writeBool(hKey, L"SerialSyncEnabled", config_.serialSyncEnabled);
            writeDword(hKey, L"SyncComPort", config_.syncComPort);
            writeBool(hKey, L"SwapEyes", config_.swapEyes);
            writeDword(hKey, L"Language", config_.language);
            writeDword(hKey, L"AiResolution", config_.aiResolution);
            writeDword(hKey, L"PhaseOffsetUs", config_.phaseOffsetUs);
            RegCloseKey(hKey);
        }
    }

private:
    FilterConfigManager() {
        InitializeCriticalSection(&cs_);
        loadFromRegistry();
    }

    ~FilterConfigManager() {
        DeleteCriticalSection(&cs_);
    }

    static void readDword(HKEY hKey, const wchar_t* name, int& value) {
        DWORD dwVal = 0;
        DWORD dwSize = sizeof(dwVal);
        if (RegQueryValueExW(hKey, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&dwVal), &dwSize) == ERROR_SUCCESS) {
            value = static_cast<int>(dwVal);
        }
    }

    static void readBool(HKEY hKey, const wchar_t* name, bool& value) {
        int v = value ? 1 : 0;
        readDword(hKey, name, v);
        value = (v != 0);
    }

    static void writeDword(HKEY hKey, const wchar_t* name, int value) {
        DWORD dwVal = static_cast<DWORD>(value);
        RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwVal), sizeof(dwVal));
    }

    static void writeBool(HKEY hKey, const wchar_t* name, bool value) {
        writeDword(hKey, name, value ? 1 : 0);
    }

    static constexpr wchar_t kRegSubKey[] = L"Software\\ParallaxRT\\Filter";

    mutable CRITICAL_SECTION cs_;
    FilterConfig config_;
    FilterTelemetry telemetry_;
    std::atomic<int> configEpoch_{0};
};

} // namespace parallaxrt
