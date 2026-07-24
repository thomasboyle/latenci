#include "TrayIcon.h"
#include "PopupWindow.h"
#include "NetworkMonitor.h"
#include "SpeedTest.h"
#include "DnsManager.h"
#include "Autostart.h"
#include "Messages.h"
#include "Fonts.h"
#include "resource.h"

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace {

HINSTANCE g_instance = nullptr;
HWND g_msgHwnd = nullptr;
TrayIcon g_tray;
PopupWindow g_popup;
NetworkMonitor g_monitor;
SpeedTest g_speed;
AppConfig g_config;
UINT_PTR g_hoverTimer = 0;
bool g_hoverPending = false;
UINT_PTR g_hoverWatchTimer = 0;
ULONGLONG g_lastTrayClickTick = 0;
SpeedTest::Result g_lastSpeedResult{};
bool g_speedTestQuiet = false;

// Shell_NotifyIconGetRect is a cross-process call into the shell. Cache it and
// refresh only when a hover begins, the popup opens, or the taskbar changes.
RECT g_trayRect{};
bool g_trayRectValid = false;

constexpr UINT_PTR kHoverTimerId = 42;
constexpr UINT_PTR kHoverWatchTimerId = 43;
constexpr ULONGLONG kTrayClickDebounceMs = 250;
// Backstop for WM_MOUSELEAVE; dismissal is not latency sensitive.
constexpr UINT kHoverWatchIntervalMs = 150;

bool RefreshTrayRect() {
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = g_msgHwnd;
    id.uID = 1;
    RECT rc{};
    if (FAILED(Shell_NotifyIconGetRect(&id, &rc))) {
        g_trayRectValid = false;
        return false;
    }
    g_trayRect = rc;
    g_trayRectValid = true;
    return true;
}

void ShowContextMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_RUN_SPEED_TEST, L"Run Speed Test");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    const UINT startupFlags = MF_STRING |
        (Autostart::IsEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, startupFlags, IDM_RUN_AT_STARTUP, L"Run at startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

POINT TrayAnchorPoint() {
    POINT pt{};
    GetCursorPos(&pt);
    if (RefreshTrayRect()) {
        pt.x = (g_trayRect.left + g_trayRect.right) / 2;
        pt.y = g_trayRect.top;
    }
    return pt;
}

bool CursorOverTray(POINT pt) {
    if (!g_trayRectValid) {
        return false;
    }
    RECT rc = g_trayRect;
    InflateRect(&rc, 8, 8);
    return PtInRect(&rc, pt) != FALSE;
}

void StopHoverWatch() {
    if (g_hoverWatchTimer && g_msgHwnd) {
        KillTimer(g_msgHwnd, g_hoverWatchTimer);
        g_hoverWatchTimer = 0;
    }
}

void StartHoverWatch() {
    if (!g_msgHwnd) {
        return;
    }
    StopHoverWatch();
    g_hoverWatchTimer = SetTimer(g_msgHwnd, kHoverWatchTimerId, kHoverWatchIntervalMs, nullptr);
}

void MaybeDismissHover() {
    if (!g_popup.IsVisible() || g_popup.IsPinned()) {
        StopHoverWatch();
        return;
    }
    POINT pt{};
    GetCursorPos(&pt);
    if (g_popup.ContainsScreenPoint(pt) || CursorOverTray(pt)) {
        return;
    }
    g_popup.Hide();
    StopHoverWatch();
}

void OpenPopupPinned() {
    g_hoverPending = false;
    if (g_hoverTimer) {
        KillTimer(g_msgHwnd, g_hoverTimer);
        g_hoverTimer = 0;
    }
    StopHoverWatch();
    if (g_popup.IsVisible() && g_popup.Mode() == ShowMode::Hover) {
        g_popup.SetPinned();
        return;
    }
    g_popup.SetSnapshot(g_monitor.GetSnapshot());
    g_popup.ShowNearTray(TrayAnchorPoint(), ShowMode::Pinned);
}

void OpenPopupHover() {
    if (g_popup.IsPinned()) {
        return;
    }
    g_popup.SetSnapshot(g_monitor.GetSnapshot());
    g_popup.ShowNearTray(TrayAnchorPoint(), ShowMode::Hover);
    StartHoverWatch();
}

void OnTrayPrimaryClick() {
    // NOTIFYICON_VERSION_4 can deliver NIN_SELECT and WM_LBUTTONUP for one click.
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTrayClickTick < kTrayClickDebounceMs) {
        return;
    }
    g_lastTrayClickTick = now;

    if (g_popup.IsPinned()) {
        g_popup.Hide();
        StopHoverWatch();
    } else {
        OpenPopupPinned();
    }
}

wchar_t* g_promptBuffer = nullptr;
size_t g_promptBufferChars = 0;
bool g_promptAccepted = false;

bool PromptCustomDns(HWND owner, wchar_t* inout, size_t inoutChars) {
    constexpr int kDlgW = 360;
    constexpr int kDlgH = 140;

    g_promptBuffer = inout;
    g_promptBufferChars = inoutChars;
    g_promptAccepted = false;

    const wchar_t* cls = L"RoutingCrumbsDnsPrompt";
    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_CREATE: {
                CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_promptBuffer,
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                16, 40, 312, 24, hwnd, reinterpret_cast<HMENU>(100), g_instance, nullptr);
                CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              160, 80, 80, 28, hwnd, reinterpret_cast<HMENU>(IDOK), g_instance, nullptr);
                CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                              248, 80, 80, 28, hwnd, reinterpret_cast<HMENU>(IDCANCEL), g_instance, nullptr);
                CreateWindowW(L"STATIC", L"Custom DNS (space or comma separated IPv4):",
                              WS_CHILD | WS_VISIBLE,
                              16, 14, 320, 20, hwnd, nullptr, g_instance, nullptr);
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == IDOK) {
                    GetDlgItemTextW(hwnd, 100, g_promptBuffer,
                                    static_cast<int>(g_promptBufferChars));
                    g_promptAccepted = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (LOWORD(wParam) == IDCANCEL) {
                    g_promptAccepted = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;
            case WM_CLOSE:
                g_promptAccepted = false;
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        };
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        s_registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner ? owner : GetDesktopWindow(), &ownerRc);
    const int x = ownerRc.left + (ownerRc.right - ownerRc.left - kDlgW) / 2;
    const int y = ownerRc.top + (ownerRc.bottom - ownerRc.top - kDlgH) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        cls,
        L"Custom DNS",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, kDlgW, kDlgH,
        owner,
        nullptr,
        g_instance,
        nullptr);

    EnableWindow(owner, FALSE);
    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    if (owner) {
        SetForegroundWindow(owner);
    }
    return g_promptAccepted;
}

void ApplyDns(DnsProvider provider) {
    if (!g_monitor.HasAdapter()) {
        MessageBoxW(g_msgHwnd, L"No active adapter found.", L"Routing Crumbs", MB_ICONWARNING);
        return;
    }

    if (provider == DnsProvider::Custom) {
        wchar_t draft[128]{};
        wcsncpy_s(draft, g_config.customDns, _TRUNCATE);
        if (!PromptCustomDns(g_popup.Hwnd() ? g_popup.Hwnd() : g_msgHwnd, draft, 128)) {
            return;
        }
        wcsncpy_s(g_config.customDns, draft, _TRUNCATE);
    }

    ErrorMsg error;
    if (!DnsManager::Apply(provider, g_monitor.CachedGuid(), g_config.customDns, error)) {
        MessageBoxW(g_msgHwnd, error.text, L"DNS change failed", MB_ICONERROR);
        return;
    }

    g_config.dnsProvider = provider;
    DnsManager::SaveConfig(g_config);
    g_popup.SetDnsProvider(provider);
}

void StartSpeedTest(bool quick = false, bool quiet = false) {
    if (g_speed.IsRunning()) {
        return;
    }
    g_speedTestQuiet = quiet;
    const auto snap = g_monitor.GetSnapshot();
    g_monitor.SetSpeedResults(snap.downloadMbps, snap.uploadMbps, true);
    g_popup.SetSpeedTestRunning(true);

    g_speed.Start(g_msgHwnd, WM_APP_SPEED_DONE, [](const SpeedTest::Result& r) {
        g_lastSpeedResult = r;
    }, quick);
}

void OnStatsUpdated() {
    g_tray.SetConnected(g_monitor.Connected());
    if (g_popup.IsVisible()) {
        g_popup.SetSnapshot(g_monitor.GetSnapshot());
    }
}

LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRAY: {
        const UINT notify = LOWORD(lParam);
        switch (notify) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            OnTrayPrimaryClick();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu(hwnd);
            break;
        case WM_MOUSEMOVE:
            if (!g_popup.IsPinned() && !g_popup.IsVisible() && !g_hoverPending) {
                // One tray-rect query per hover attempt, not per watchdog tick.
                RefreshTrayRect();
                g_hoverPending = true;
                g_hoverTimer = SetTimer(hwnd, kHoverTimerId, Theme::kHoverOpenDelayMs, nullptr);
            }
            break;
        case NIN_POPUPCLOSE:
            MaybeDismissHover();
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
        g_trayRectValid = false;
        return 0;

    case WM_APP_HIDE_POPUP:
        MaybeDismissHover();
        return 0;

    case WM_TIMER:
        if (wParam == kHoverTimerId) {
            KillTimer(hwnd, kHoverTimerId);
            g_hoverTimer = 0;
            g_hoverPending = false;
            POINT pt{};
            GetCursorPos(&pt);
            if (CursorOverTray(pt) && !g_popup.IsVisible() && !g_popup.IsPinned()) {
                OpenPopupHover();
            }
        } else if (wParam == kHoverWatchTimerId) {
            MaybeDismissHover();
        }
        return 0;

    case WM_APP_STATS_UPDATED:
        OnStatsUpdated();
        return 0;

    case WM_APP_SPEED_DONE: {
        if (wParam) {
            g_monitor.SetSpeedResults(
                g_lastSpeedResult.downloadMbps,
                g_lastSpeedResult.uploadMbps,
                false);
        } else {
            const auto snap = g_monitor.GetSnapshot();
            g_monitor.SetSpeedResults(snap.downloadMbps, snap.uploadMbps, false);
            if (!g_speedTestQuiet && !g_lastSpeedResult.error.Empty()) {
                MessageBoxW(hwnd, g_lastSpeedResult.error.text,
                            L"Speed test failed", MB_ICONWARNING);
            }
        }
        g_speedTestQuiet = false;
        g_popup.SetSpeedTestRunning(false);
        g_popup.SetSnapshot(g_monitor.GetSnapshot());
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_RUN_SPEED_TEST:
            StartSpeedTest(false, false);
            if (!g_popup.IsVisible()) {
                OpenPopupPinned();
            }
            break;
        case IDM_SETTINGS:
            MessageBoxW(hwnd,
                        L"Settings are limited to DNS provider selection in the flyout.\n"
                        L"Ping target and preferences live under HKCU\\Software\\RoutingCrumbs.",
                        L"Routing Crumbs",
                        MB_ICONINFORMATION);
            break;
        case IDM_RUN_AT_STARTUP: {
            const bool enable = !Autostart::IsEnabled();
            ErrorMsg error;
            if (!Autostart::SetEnabled(enable, error)) {
                MessageBoxW(hwnd,
                            error.Empty() ? L"Could not update startup setting."
                                          : error.text,
                            L"Routing Crumbs",
                            MB_ICONWARNING);
            }
            break;
        }
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        if (g_hoverTimer) {
            KillTimer(hwnd, g_hoverTimer);
            g_hoverTimer = 0;
        }
        StopHoverWatch();
        g_speed.Cancel();
        g_monitor.Stop();
        g_popup.Destroy();
        g_tray.Destroy();
        UnloadAppFonts();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    LoadAppFonts(instance);

    // Winsock for InetNtop / GetAddrInfo
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    DnsManager::LoadConfig(g_config);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MessageWndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RoutingCrumbsMsg";
    RegisterClassExW(&wc);

    g_msgHwnd = CreateWindowExW(
        0,
        L"RoutingCrumbsMsg",
        L"Routing Crumbs",
        WS_OVERLAPPED,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!g_msgHwnd) {
        return 1;
    }

    if (!g_tray.Create(instance, g_msgHwnd, WM_APP_TRAY)) {
        MessageBoxW(nullptr, L"Failed to create tray icon.", L"Routing Crumbs", MB_ICONERROR);
        return 1;
    }

    if (!g_popup.Create(instance, g_msgHwnd)) {
        MessageBoxW(nullptr, L"Failed to create popup window.", L"Routing Crumbs", MB_ICONERROR);
        return 1;
    }

    g_popup.SetDnsProvider(g_config.dnsProvider);
    g_popup.SetDnsClickHandler([](DnsProvider p) { ApplyDns(p); });
    g_popup.SetSpeedClickHandler([]() { StartSpeedTest(false, false); });
    g_popup.SetVisibilityHandler([](bool visible) {
        g_monitor.SetLiveUpdates(visible);
    });

    g_monitor.SetPingTarget(g_config.pingTarget);
    g_monitor.Start(g_msgHwnd, WM_APP_STATS_UPDATED);

    // Populate Download/Upload once at launch (background, quiet on failure).
    StartSpeedTest(true, true);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return static_cast<int>(msg.wParam);
}
