#pragma once

#include "core/filter_config.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

namespace parallaxrt {

class TrayController {
public:
    static TrayController& instance() {
        static TrayController inst;
        return inst;
    }

    void start(HINSTANCE hInst = nullptr) {
        if (running_.exchange(true)) {
            return;
        }
        hInst_ = (hInst != nullptr) ? hInst : GetModuleHandleW(nullptr);
        thread_ = std::thread(&TrayController::messageLoop, this);
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (msgHwnd_ != nullptr) {
            PostMessageW(msgHwnd_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.detach();
        }
    }

    void showSettingsDialog() {
        if (msgHwnd_ != nullptr) {
            PostMessageW(msgHwnd_, kMsgOpenSettings, 0, 0);
        }
    }

private:
    static constexpr UINT kTrayIconMsg = WM_USER + 101;
    static constexpr UINT kMsgOpenSettings = WM_USER + 102;
    static constexpr UINT kTrayIconId = 1;

    static constexpr int kMenuOpenSettings = 2001;
    static constexpr int kMenuToggleFilter = 2002;
    static constexpr int kMenuExit = 2003;

    static constexpr int kLangCombo = 3000;
    static constexpr int kModeCombo = 3001;
    static constexpr int kModelCombo = 3002;
    static constexpr int kDisparitySlider = 3003;
    static constexpr int kFocusSlider = 3004;
    static constexpr int kDetailSlider = 3005;
    static constexpr int kDetailCheck = 3006;
    static constexpr int kTemporalCheck = 3007;
    static constexpr int kSubtitleCheck = 3008;
    static constexpr int kAntiFlickerCheck = 3009;
    static constexpr int kFilterEnabledCheck = 3010;
    static constexpr int kSerialSyncCheck = 3011;
    static constexpr int kComPortCombo = 3012;
    static constexpr int kCloseButton = 3014;
    static constexpr int kSwapEyesCheck = 3015;
    static constexpr int kResetButton = 3016;
    static constexpr int kResCombo = 3017;
    static constexpr int kPhaseSlider = 3018;

    TrayController() = default;
    ~TrayController() { stop(); }

    void messageLoop() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayController::msgWindowProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = L"ParallaxRTTrayMsgWindowClass";
        RegisterClassExW(&wc);

        // Windows Shell_NotifyIcon requires a normal top-level window (not HWND_MESSAGE) to receive tray events reliably
        msgHwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, wc.lpszClassName, L"ParallaxRT Tray Message Receiver",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst_, this);

        addTrayIcon();

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        removeTrayIcon();
        if (settingsHwnd_ != nullptr) {
            DestroyWindow(settingsHwnd_);
            settingsHwnd_ = nullptr;
        }
        DestroyWindow(msgHwnd_);
        msgHwnd_ = nullptr;
        UnregisterClassW(wc.lpszClassName, hInst_);
    }

    void addTrayIcon() {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = msgHwnd_;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayIconMsg;
        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&TrayController::instance), &hModule);
        const int cxSm = GetSystemMetrics(SM_CXSMICON);
        const int cySm = GetSystemMetrics(SM_CYSMICON);
        HICON hAppIcon = static_cast<HICON>(LoadImageW(
            hModule ? hModule : hInst_,
            MAKEINTRESOURCEW(101),
            IMAGE_ICON,
            cxSm, cySm,
            LR_DEFAULTCOLOR));
        if (!hAppIcon) hAppIcon = LoadIconW(hModule ? hModule : hInst_, MAKEINTRESOURCEW(101));
        if (!hAppIcon) hAppIcon = LoadIconW(nullptr, IDI_APPLICATION);
        nid.hIcon = hAppIcon;
        const auto cfg = FilterConfigManager::instance().getConfig();
        wcscpy_s(nid.szTip, cfg.language == 1
            ? L"ParallaxRT - PotPlayer 实时3D滤镜"
            : L"ParallaxRT - Real-Time 3D Video Filter");
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void removeTrayIcon() {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = msgHwnd_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void showContextMenu() {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        if (hMenu == nullptr) return;

        const auto cfg = FilterConfigManager::instance().getConfig();
        const bool isZh = (cfg.language == 1);

        AppendMenuW(hMenu, MF_STRING, kMenuOpenSettings,
                    isZh ? L"打开滤镜设置(&S)…" : L"Open Settings(&S)…");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING | (cfg.filterEnabled ? MF_CHECKED : MF_UNCHECKED),
                    kMenuToggleFilter,
                    isZh ? L"启用 3D 转换滤镜" : L"Enable 3D Stereo Filter");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, kMenuExit,
                    isZh ? L"退出滤镜(&X)" : L"Exit Filter(&X)");

        SetForegroundWindow(msgHwnd_);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, msgHwnd_, nullptr);
        DestroyMenu(hMenu);
    }

    void createOrShowSettingsWindow() {
        if (settingsHwnd_ != nullptr && IsWindow(settingsHwnd_)) {
            ShowWindow(settingsHwnd_, SW_RESTORE);
            SetForegroundWindow(settingsHwnd_);
            return;
        }

        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&TrayController::instance), &hModule);
        const int cxSm = GetSystemMetrics(SM_CXSMICON);
        const int cySm = GetSystemMetrics(SM_CYSMICON);
        const int cxLg = GetSystemMetrics(SM_CXICON);
        const int cyLg = GetSystemMetrics(SM_CYICON);
        HICON hIconLg = static_cast<HICON>(LoadImageW(
            hModule ? hModule : hInst_, MAKEINTRESOURCEW(101), IMAGE_ICON, cxLg, cyLg, LR_DEFAULTCOLOR));
        HICON hIconSm = static_cast<HICON>(LoadImageW(
            hModule ? hModule : hInst_, MAKEINTRESOURCEW(101), IMAGE_ICON, cxSm, cySm, LR_DEFAULTCOLOR));
        if (!hIconLg) hIconLg = LoadIconW(hModule ? hModule : hInst_, MAKEINTRESOURCEW(101));
        if (!hIconLg) hIconLg = LoadIconW(nullptr, IDI_APPLICATION);
        if (!hIconSm) hIconSm = hIconLg;

        WNDCLASSEXW swc{};
        swc.cbSize = sizeof(swc);
        swc.lpfnWndProc = &TrayController::settingsWindowProc;
        swc.hInstance = hInst_;
        swc.hIcon = hIconLg;
        swc.hIconSm = hIconSm;
        swc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        swc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        swc.lpszClassName = L"ParallaxRTSettingsWindowClass";
        RegisterClassExW(&swc);

        settingsHwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, swc.lpszClassName,
            L"ParallaxRT - 实时 3D 滤镜设置",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 460, 680,
            nullptr, nullptr, hInst_, this);

        // Center on screen
        RECT rc{};
        GetWindowRect(settingsHwnd_, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(settingsHwnd_, HWND_TOPMOST, (screenW - w) / 2, (screenH - h) / 2, w, h, SWP_SHOWWINDOW);
    }

    static LRESULT CALLBACK msgWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<TrayController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (uMsg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<TrayController*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self != nullptr) {
            if (uMsg == kTrayIconMsg) {
                if (lParam == WM_RBUTTONUP) {
                    self->showContextMenu();
                    return 0;
                } else if (lParam == WM_LBUTTONDBLCLK) {
                    self->createOrShowSettingsWindow();
                    return 0;
                }
            } else if (uMsg == kMsgOpenSettings) {
                self->createOrShowSettingsWindow();
                return 0;
            } else if (uMsg == WM_COMMAND) {
                switch (LOWORD(wParam)) {
                    case kMenuOpenSettings:
                        self->createOrShowSettingsWindow();
                        return 0;
                    case kMenuToggleFilter: {
                        auto cfg = FilterConfigManager::instance().getConfig();
                        cfg.filterEnabled = !cfg.filterEnabled;
                        FilterConfigManager::instance().setConfig(cfg);
                        return 0;
                    }
                    case kMenuExit: {
                        auto cfg = FilterConfigManager::instance().getConfig();
                        cfg.filterEnabled = false;
                        FilterConfigManager::instance().setConfig(cfg);
                        self->stop();
                        return 0;
                    }
                }
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    struct SettingsControls {
        HWND langLabel = nullptr;
        HWND langCombo = nullptr;
        HWND modeLabel = nullptr;
        HWND modeCombo = nullptr;
        HWND modelLabel = nullptr;
        HWND modelCombo = nullptr;
        HWND resLabel = nullptr;
        HWND resCombo = nullptr;
        HWND disparityLabel = nullptr;
        HWND disparitySlider = nullptr;
        HWND disparityVal = nullptr;
        HWND focusLabel = nullptr;
        HWND focusSlider = nullptr;
        HWND focusVal = nullptr;
        HWND detailLabel = nullptr;
        HWND detailSlider = nullptr;
        HWND detailVal = nullptr;
        HWND detailCheck = nullptr;
        HWND temporalCheck = nullptr;
        HWND subtitleCheck = nullptr;
        HWND antiFlickerCheck = nullptr;
        HWND enabledCheck = nullptr;
        HWND serialSyncCheck = nullptr;
        HWND comPortLabel = nullptr;
        HWND comPortCombo = nullptr;
        HWND swapEyesCheck = nullptr;
        HWND phaseLabel = nullptr;
        HWND phaseSlider = nullptr; 
        HWND phaseVal = nullptr;
        HWND statusGroup = nullptr;
        HWND statusFpsLabel = nullptr;
        HWND statusStateLabel = nullptr;
        HWND btnReset = nullptr;
        HWND btnClose = nullptr;
    };

    static SettingsControls& getControls() {
        static SettingsControls ctrl;
        return ctrl;
    }

    static HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
        HWND lbl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  x, y, w, h, parent, nullptr, nullptr, nullptr);
        SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return lbl;
    }

    static void applyCurrentUiToConfig() {
        auto& ctrl = getControls();
        if (!ctrl.modeCombo || !IsWindow(ctrl.modeCombo)) return;

        FilterConfig cfg = FilterConfigManager::instance().getConfig();
        const int langSel = static_cast<int>(SendMessageW(ctrl.langCombo, CB_GETCURSEL, 0, 0));
        if (langSel >= 0) cfg.language = langSel;

        const int modeSel = static_cast<int>(SendMessageW(ctrl.modeCombo, CB_GETCURSEL, 0, 0));
        if (modeSel >= 0) cfg.mode = modeSel;

        const int modelSel = static_cast<int>(SendMessageW(ctrl.modelCombo, CB_GETCURSEL, 0, 0));
        if (modelSel >= 0) cfg.modelSelection = modelSel;

        const int resSel = static_cast<int>(SendMessageW(ctrl.resCombo, CB_GETCURSEL, 0, 0));
        if (resSel >= 0) cfg.aiResolution = resSel;

        cfg.maxDisparity = static_cast<int>(SendMessageW(ctrl.disparitySlider, TBM_GETPOS, 0, 0));
        cfg.focusPercent = static_cast<int>(SendMessageW(ctrl.focusSlider, TBM_GETPOS, 0, 0));
        cfg.detailStrengthPercent = static_cast<int>(SendMessageW(ctrl.detailSlider, TBM_GETPOS, 0, 0));
        cfg.enableRgbDetail = (SendMessageW(ctrl.detailCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.enableTemporalReuse = (SendMessageW(ctrl.temporalCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.antiFlicker = (SendMessageW(ctrl.antiFlickerCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.protectSubtitles = (SendMessageW(ctrl.subtitleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.filterEnabled = (SendMessageW(ctrl.enabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.serialSyncEnabled = (SendMessageW(ctrl.serialSyncCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
        cfg.swapEyes = (SendMessageW(ctrl.swapEyesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

        if (ctrl.phaseSlider && IsWindow(ctrl.phaseSlider)) {
            const int phaseRaw = static_cast<int>(SendMessageW(ctrl.phaseSlider, TBM_GETPOS, 0, 0));
            cfg.phaseOffsetUs = phaseRaw - 4000;
        }

        const int portIdx = static_cast<int>(SendMessageW(ctrl.comPortCombo, CB_GETCURSEL, 0, 0));
        if (portIdx >= 0) {
            const LRESULT portVal = SendMessageW(ctrl.comPortCombo, CB_GETITEMDATA, portIdx, 0);
            cfg.syncComPort = (portVal != CB_ERR && portVal > 0) ? static_cast<int>(portVal) : (portIdx + 1);
        }

        FilterConfigManager::instance().setConfig(cfg);
    }

    static void refreshLocalizedTexts(HWND hwnd) {
        auto& ctrl = getControls();
        const auto cfg = FilterConfigManager::instance().getConfig();
        const bool isZh = (cfg.language == 1);

        SetWindowTextW(hwnd, isZh ? L"ParallaxRT - 实时 3D 滤镜设置" : L"ParallaxRT - Real-Time 3D Filter Settings");

        if (ctrl.langLabel) SetWindowTextW(ctrl.langLabel, isZh ? L"界面语言:" : L"Language:");
        if (ctrl.modeLabel) SetWindowTextW(ctrl.modeLabel, isZh ? L"3D 画面格式:" : L"3D Format:");
        if (ctrl.modelLabel) SetWindowTextW(ctrl.modelLabel, isZh ? L"AI 深度模型:" : L"AI Depth Model:");
        if (ctrl.disparityLabel) SetWindowTextW(ctrl.disparityLabel, isZh ? L"最大视差深度:" : L"Max Disparity:");
        if (ctrl.focusLabel) SetWindowTextW(ctrl.focusLabel, isZh ? L"屏幕汇聚平面:" : L"Convergence Plane:");
        if (ctrl.detailLabel) SetWindowTextW(ctrl.detailLabel, isZh ? L"微面部细节度:" : L"Detail Strength:");

        if (ctrl.detailCheck) SetWindowTextW(ctrl.detailCheck, isZh
            ? L"启用 RGB 原图高频微浮雕注入"
            : L"Inject High-Frequency RGB Bas-Relief Detail");
        if (ctrl.temporalCheck) SetWindowTextW(ctrl.temporalCheck, isZh
            ? L"隔帧推理 (1:2 深度复用，大幅降低 GPU 占用)"
            : L"Temporal Frame Reuse (1:2 Depth Reuse, Lowers GPU Load)");
        if (ctrl.antiFlickerCheck) SetWindowTextW(ctrl.antiFlickerCheck, isZh
            ? L"去闪烁平滑滤波与时间平滑 (消除时序抖动)"
            : L"Anti-Flicker Temporal Depth Smoothing (Jitter Suppression)");
        if (ctrl.subtitleCheck) SetWindowTextW(ctrl.subtitleCheck, isZh
            ? L"字幕与电视台标防撕裂保护 (高对比平整贴合)"
            : L"Subtitle & Logo Tear Protection (High-Contrast Flatting)");
        if (ctrl.enabledCheck) SetWindowTextW(ctrl.enabledCheck, isZh
            ? L"启用 PotPlayer 实时 2D 转 3D 渲染 (全局开关)"
            : L"Enable Real-Time 2D to 3D Stereo Filter (Master Switch)");
        if (ctrl.serialSyncCheck) SetWindowTextW(ctrl.serialSyncCheck, isZh
            ? L"快门式3D硬件同步 (DLP-Link)"
            : L"Shutter 3D Hardware Sync (DLP-Link Transmitter)");
        if (ctrl.comPortLabel) SetWindowTextW(ctrl.comPortLabel, isZh ? L"串口:" : L"Port:");
        if (ctrl.swapEyesCheck) SetWindowTextW(ctrl.swapEyesCheck, isZh
            ? L"翻转左右眼 (Swap Left/Right Eyes 解决景深反转)"
            : L"Swap Left / Right Eyes (Fix Inverted / Pseudoscopic Depth)");

        if (ctrl.phaseLabel) SetWindowTextW(ctrl.phaseLabel,
            isZh ? L"同步相位微调:" : L"Sync Phase:");
            
        if (ctrl.statusGroup) SetWindowTextW(ctrl.statusGroup, isZh ? L"实时运行状态" : L"Live Engine Status");
        if (ctrl.btnReset) SetWindowTextW(ctrl.btnReset, isZh ? L"恢复默认值" : L"Reset Defaults");
        if (ctrl.btnClose) SetWindowTextW(ctrl.btnClose, isZh ? L"关闭" : L"Close");

        // Refresh mode combo items
        const int curMode = static_cast<int>(SendMessageW(ctrl.modeCombo, CB_GETCURSEL, 0, 0));
        SendMessageW(ctrl.modeCombo, CB_RESETCONTENT, 0, 0);
        if (isZh) {
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"红青眼镜 (Anaglyph)"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左右分屏 Half-SBS"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左右分屏 Full-SBS"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"实时深度图 (灰度)"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"上下格式 (Top-and-Bottom)"));
        } else {
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Red-Cyan Anaglyph"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side-by-Side (Half-SBS)"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side-by-Side (Full-SBS)"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Map (Grayscale)"));
            SendMessageW(ctrl.modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-and-Bottom (TAB)"));
        }
        SendMessageW(ctrl.modeCombo, CB_SETCURSEL, (curMode >= 0 && curMode <= 4) ? curMode : cfg.mode, 0);

        // Refresh model combo items
        const int curModel = static_cast<int>(SendMessageW(ctrl.modelCombo, CB_GETCURSEL, 0, 0));
        SendMessageW(ctrl.modelCombo, CB_RESETCONTENT, 0, 0);
        if (isZh) {
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Base - 高精)"));
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Small - 极速)"));
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Large - 旗舰/4070Ti+)"));
        } else {
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Base - High Quality)"));
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Small - Ultra Fast)"));
            SendMessageW(ctrl.modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Depth Anything V2 (Large - Flagship/RTX 4070Ti+)"));
        }
        SendMessageW(ctrl.modelCombo, CB_SETCURSEL, (curModel >= 0 && curModel <= 2) ? curModel : cfg.modelSelection, 0);

        updateLabels();
    }

    static LRESULT CALLBACK settingsWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<TrayController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (uMsg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<TrayController*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        auto& ctrl = getControls();

        switch (uMsg) {
            case WM_CREATE: {
                INITCOMMONCONTROLSEX icex{};
                icex.dwSize = sizeof(icex);
                icex.dwICC = ICC_BAR_CLASSES;
                InitCommonControlsEx(&icex);

                const auto cfg = FilterConfigManager::instance().getConfig();
                const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

                // 1. Language selector
                ctrl.langLabel = createLabel(hwnd, L"Language:", 24, 18, 115, 22);
                ctrl.langCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    145, 15, 275, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLangCombo)), nullptr, nullptr);
                SendMessageW(ctrl.langCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.langCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English (英语)"));
                SendMessageW(ctrl.langCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文 (Simplified Chinese)"));
                SendMessageW(ctrl.langCombo, CB_SETCURSEL, (cfg.language == 1 ? 1 : 0), 0);

                // 2. 3D Format
                ctrl.modeLabel = createLabel(hwnd, L"3D Format:", 24, 52, 115, 22);
                ctrl.modeCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    145, 49, 275, 180, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kModeCombo)), nullptr, nullptr);
                SendMessageW(ctrl.modeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                // 3. AI Model
                ctrl.modelLabel = createLabel(hwnd, L"AI Depth Model:", 24, 86, 115, 22);
                ctrl.modelCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    145, 83, 275, 180, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kModelCombo)), nullptr, nullptr);
                SendMessageW(ctrl.modelCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                // 4. Max Disparity Slider
                // 3.5 AI Compute Resolution
                ctrl.resLabel = createLabel(hwnd, L"AI Resolution:", 24, 120, 115, 22);
                ctrl.resCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    145, 117, 275, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResCombo)), nullptr, nullptr);
                SendMessageW(ctrl.resCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.resCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"518x518 (High Quality)"));
                SendMessageW(ctrl.resCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"392x392 (Balanced)"));
                SendMessageW(ctrl.resCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"252x252 (Fastest)"));
                SendMessageW(ctrl.resCombo, CB_SETCURSEL, cfg.aiResolution, 0);

                ctrl.disparityLabel = createLabel(hwnd, L"Max Disparity:", 24, 156, 115, 22);
                ctrl.disparitySlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                    145, 152, 205, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisparitySlider)), nullptr, nullptr);
                SendMessageW(ctrl.disparitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 120));
                SendMessageW(ctrl.disparitySlider, TBM_SETTICFREQ, 10, 0);
                SendMessageW(ctrl.disparitySlider, TBM_SETPOS, TRUE, cfg.maxDisparity);
                ctrl.disparityVal = createLabel(hwnd, L"", 360, 156, 60, 22);

                // 5. Focus Plane Slider
                ctrl.focusLabel = createLabel(hwnd, L"Convergence:", 24, 192, 115, 22);
                ctrl.focusSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                    145, 188, 205, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFocusSlider)), nullptr, nullptr);
                SendMessageW(ctrl.focusSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
                SendMessageW(ctrl.focusSlider, TBM_SETTICFREQ, 10, 0);
                SendMessageW(ctrl.focusSlider, TBM_SETPOS, TRUE, cfg.focusPercent);
                ctrl.focusVal = createLabel(hwnd, L"", 360, 192, 60, 22);

                // 6. Detail Strength Slider
                ctrl.detailLabel = createLabel(hwnd, L"Detail Strength:", 24, 228, 115, 22);
                ctrl.detailSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                    145, 224, 205, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailSlider)), nullptr, nullptr);
                SendMessageW(ctrl.detailSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 60));
                SendMessageW(ctrl.detailSlider, TBM_SETTICFREQ, 10, 0);
                SendMessageW(ctrl.detailSlider, TBM_SETPOS, TRUE, cfg.detailStrengthPercent);
                ctrl.detailVal = createLabel(hwnd, L"", 360, 228, 60, 22);

                // Checkboxes
                ctrl.detailCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 266, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailCheck)), nullptr, nullptr);
                SendMessageW(ctrl.detailCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.detailCheck, BM_SETCHECK, cfg.enableRgbDetail ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.temporalCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 294, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTemporalCheck)), nullptr, nullptr);
                SendMessageW(ctrl.temporalCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.temporalCheck, BM_SETCHECK, cfg.enableTemporalReuse ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.antiFlickerCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 322, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAntiFlickerCheck)), nullptr, nullptr);
                SendMessageW(ctrl.antiFlickerCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.antiFlickerCheck, BM_SETCHECK, cfg.antiFlicker ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.subtitleCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 350, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSubtitleCheck)), nullptr, nullptr);
                SendMessageW(ctrl.subtitleCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.subtitleCheck, BM_SETCHECK, cfg.protectSubtitles ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.enabledCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 378, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterEnabledCheck)), nullptr, nullptr);
                SendMessageW(ctrl.enabledCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.enabledCheck, BM_SETCHECK, cfg.filterEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.serialSyncCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 406, 290, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSerialSyncCheck)), nullptr, nullptr);
                SendMessageW(ctrl.serialSyncCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctrl.serialSyncCheck, BM_SETCHECK, cfg.serialSyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

                ctrl.comPortLabel = createLabel(hwnd, L"Port:", 320, 408, 35, 22);
                ctrl.comPortCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                    355, 404, 65, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComPortCombo)), nullptr, nullptr);
                SendMessageW(ctrl.comPortCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                // Dynamically enumerate COM ports from Windows Registry + include configured port
                std::set<int> availablePorts;
                HKEY hSerialComm = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hSerialComm) == ERROR_SUCCESS) {
                    DWORD valIndex = 0;
                    wchar_t valName[256];
                    BYTE valData[256];
                    DWORD nameLen = 256;
                    DWORD dataLen = 256;
                    DWORD type = 0;
                    while (RegEnumValueW(hSerialComm, valIndex++, valName, &nameLen, nullptr, &type, valData, &dataLen) == ERROR_SUCCESS) {
                        if (type == REG_SZ) {
                            wchar_t* portStr = reinterpret_cast<wchar_t*>(valData);
                            int portNum = 0;
                            if (swscanf_s(portStr, L"COM%d", &portNum) == 1 && portNum > 0) {
                                availablePorts.insert(portNum);
                            }
                        }
                        nameLen = 256;
                        dataLen = 256;
                    }
                    RegCloseKey(hSerialComm);
                }

                if (cfg.syncComPort > 0) {
                    availablePorts.insert(cfg.syncComPort);
                }
                if (availablePorts.empty()) {
                    for (int p = 1; p <= 64; ++p) {
                        availablePorts.insert(p);
                    }
                }

                int curPortSel = 0;
                int itemIdx = 0;
                for (int portNum : availablePorts) {
                    wchar_t portBuf[32];
                    swprintf_s(portBuf, L"COM%d", portNum);
                    const LRESULT addedIdx = SendMessageW(ctrl.comPortCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(portBuf));
                    SendMessageW(ctrl.comPortCombo, CB_SETITEMDATA, addedIdx, static_cast<LPARAM>(portNum));
                    if (portNum == cfg.syncComPort) {
                        curPortSel = itemIdx;
                    }
                    itemIdx++;
                }
                SendMessageW(ctrl.comPortCombo, CB_SETCURSEL, curPortSel, 0);

                // Swap Left / Right Eyes
                ctrl.swapEyesCheck = CreateWindowExW(0, L"BUTTON", L"",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                    24, 434, 395, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSwapEyesCheck)), nullptr, nullptr);
                SendMessageW(ctrl.swapEyesCheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                                SendMessageW(ctrl.swapEyesCheck, BM_SETCHECK, cfg.swapEyes ? BST_CHECKED : BST_UNCHECKED, 0);

                // === NEW: DLP-Link pulse phase fine-tune slider ===
                ctrl.phaseLabel = createLabel(hwnd, L"Sync Phase:", 24, 466, 115, 22);
                ctrl.phaseSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                    145, 462, 205, 32, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPhaseSlider)), nullptr, nullptr);
                SendMessageW(ctrl.phaseSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 8000)); // 0..8000 → -4000..+4000 us
                SendMessageW(ctrl.phaseSlider, TBM_SETTICFREQ, 500, 0);
                SendMessageW(ctrl.phaseSlider, TBM_SETPOS, TRUE, cfg.phaseOffsetUs + 4000);
                ctrl.phaseVal = createLabel(hwnd, L"", 360, 466, 60, 22);

                // Real-time Status & Telemetry Group Box（整体下移 36px）
                ctrl.statusGroup = CreateWindowExW(0, L"BUTTON", L"实时运行状态",
                    WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                    24, 498, 395, 62, hwnd, nullptr, nullptr, nullptr);
                SendMessageW(ctrl.statusGroup, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                ctrl.statusFpsLabel = createLabel(hwnd, L"渲染帧率: -- FPS", 38, 518, 180, 20);
                ctrl.statusStateLabel = createLabel(hwnd, L"状态: 空闲 (等待播放)", 38, 538, 365, 20);

                // Reset & Close Buttons
                ctrl.btnReset = CreateWindowExW(0, L"BUTTON", L"Reset Defaults",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                    24, 572, 130, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetButton)), nullptr, nullptr);
                SendMessageW(ctrl.btnReset, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                ctrl.btnClose = CreateWindowExW(0, L"BUTTON", L"Close",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    320, 572, 100, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)), nullptr, nullptr);
                SendMessageW(ctrl.btnClose, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                // 250ms periodic timer to update live FPS and playback stats
                SetTimer(hwnd, 1, 250, nullptr);

                refreshLocalizedTexts(hwnd);
                updateTelemetryDisplay(hwnd);
                return 0;
            }

            case WM_TIMER:
                updateTelemetryDisplay(hwnd);
                return 0;

            case WM_HSCROLL:
                updateLabels();
                applyCurrentUiToConfig();
                return 0;

            case WM_COMMAND: {
                const int controlId = LOWORD(wParam);
                const int notifCode = HIWORD(wParam);

                if (controlId == kResetButton) {
                    FilterConfigManager::instance().resetToDefaults();
                    const auto defCfg = FilterConfigManager::instance().getConfig();
                    SendMessageW(ctrl.modeCombo, CB_SETCURSEL, defCfg.mode, 0);
                    SendMessageW(ctrl.modelCombo, CB_SETCURSEL, defCfg.modelSelection, 0);
                    SendMessageW(ctrl.disparitySlider, TBM_SETPOS, TRUE, defCfg.maxDisparity);
                    SendMessageW(ctrl.focusSlider, TBM_SETPOS, TRUE, defCfg.focusPercent);
                    SendMessageW(ctrl.detailSlider, TBM_SETPOS, TRUE, defCfg.detailStrengthPercent);
                    SendMessageW(ctrl.detailCheck, BM_SETCHECK, defCfg.enableRgbDetail ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.temporalCheck, BM_SETCHECK, defCfg.enableTemporalReuse ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.antiFlickerCheck, BM_SETCHECK, defCfg.antiFlicker ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.subtitleCheck, BM_SETCHECK, defCfg.protectSubtitles ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.enabledCheck, BM_SETCHECK, defCfg.filterEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.serialSyncCheck, BM_SETCHECK, defCfg.serialSyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.swapEyesCheck, BM_SETCHECK, defCfg.swapEyes ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(ctrl.phaseSlider, TBM_SETPOS, TRUE, defCfg.phaseOffsetUs + 4000);  // NEW
                    updateLabels();
                    updateLabels();
                    return 0;
                }

                if (controlId == kCloseButton) {
                    applyCurrentUiToConfig();
                    DestroyWindow(hwnd);
                    return 0;
                }

                if (controlId == kLangCombo && notifCode == CBN_SELCHANGE) {
                    applyCurrentUiToConfig();
                    refreshLocalizedTexts(hwnd);
                    return 0;
                }

                // Combo box changes
                if ((controlId == kModeCombo || controlId == kModelCombo || controlId == kResCombo || controlId == kComPortCombo) &&
                    notifCode == CBN_SELCHANGE) {
                    applyCurrentUiToConfig();
                    return 0;
                }

                // Checkbox toggles (BN_CLICKED is 0)
                if (controlId == kDetailCheck || controlId == kTemporalCheck ||
                    controlId == kSubtitleCheck || controlId == kAntiFlickerCheck ||
                    controlId == kFilterEnabledCheck || controlId == kSerialSyncCheck ||
                    controlId == kSwapEyesCheck) {
                    applyCurrentUiToConfig();
                    return 0;
                }
                break;
            }

            case WM_CLOSE:
                applyCurrentUiToConfig();
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                if (self != nullptr) {
                    self->settingsHwnd_ = nullptr;
                }
                return 0;
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    static void updateLabels() {
        auto& ctrl = getControls();
        const int disp = static_cast<int>(SendMessageW(ctrl.disparitySlider, TBM_GETPOS, 0, 0));
        const int focus = static_cast<int>(SendMessageW(ctrl.focusSlider, TBM_GETPOS, 0, 0));
        const int detail = static_cast<int>(SendMessageW(ctrl.detailSlider, TBM_GETPOS, 0, 0));

        wchar_t buf[32];
        swprintf_s(buf, L"%d px", disp);
        SetWindowTextW(ctrl.disparityVal, buf);

        swprintf_s(buf, L"%d%%", focus);
        SetWindowTextW(ctrl.focusVal, buf);

        swprintf_s(buf, L"%d%%", detail);
        SetWindowTextW(ctrl.detailVal, buf);

        // NEW: phase offset label
        if (ctrl.phaseSlider && IsWindow(ctrl.phaseSlider)) {
            const int phaseRaw = static_cast<int>(SendMessageW(ctrl.phaseSlider, TBM_GETPOS, 0, 0));
            const int phaseUs = phaseRaw - 4000;
            swprintf_s(buf, L"%+d us", phaseUs);
            SetWindowTextW(ctrl.phaseVal, buf);
        }
    }

    static void updateTelemetryDisplay(HWND hwnd) {
        (void)hwnd;
        auto& ctrl = getControls();
        if (!ctrl.statusFpsLabel || !IsWindow(ctrl.statusFpsLabel)) return;

        const auto cfg = FilterConfigManager::instance().getConfig();
        const auto telem = FilterConfigManager::instance().getTelemetry();
        const bool isZh = (cfg.language == 1);

        const ULONGLONG nowTick = GetTickCount64();
        const bool isActive = (telem.lastActiveTick > 0 && (nowTick - telem.lastActiveTick) < 1500);

        wchar_t fpsBuf[64];
        if (isActive && telem.currentFps > 0.05) {
            swprintf_s(fpsBuf, isZh ? L"渲染帧率: %.1f FPS" : L"Render FPS: %.1f FPS", telem.currentFps);
        } else {
            swprintf_s(fpsBuf, isZh ? L"渲染帧率: -- FPS" : L"Render FPS: -- FPS");
        }
        SetWindowTextW(ctrl.statusFpsLabel, fpsBuf);

        wchar_t stateBuf[128];
        if (!isActive) {
            swprintf_s(stateBuf, isZh ? L"状态: 空闲 (播放器未启动或已暂停)" : L"Status: Idle (Player stopped / paused)");
        } else if (!telem.aiActive) {
            swprintf_s(stateBuf, isZh ? L"状态: 2D 原画直通 (滤镜已旁路关闭)" : L"Status: 2D Passthrough (Bypassed)");
        } else {
            swprintf_s(stateBuf, isZh
                ? L"状态: 实时 3D 渲染中 (%dx%d)"
                : L"Status: Real-Time 3D Active (%dx%d)",
                telem.inputWidth, telem.inputHeight);
        }
        SetWindowTextW(ctrl.statusStateLabel, stateBuf);
    }

    HINSTANCE hInst_ = nullptr;
    HWND msgHwnd_ = nullptr;
    HWND settingsHwnd_ = nullptr;
    std::atomic_bool running_{false};
    std::thread thread_;
};

} // namespace parallaxrt
