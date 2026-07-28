#include "PopupWindow.h"
#include "Fonts.h"
#include "Version.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr wchar_t kPopupClass[] = L"LatenciPopup";

constexpr wchar_t kSectionSpeedTest[] = L"Speed Test";
constexpr wchar_t kSectionDns[] = L"DNS Provider";

// Grain is a wrapped tile rather than a full-panel surface; dot count keeps the
// original 420-dots-per-panel density.
constexpr UINT kGrainTile = 128;
constexpr unsigned kGrainDots = 45;

D2D1_COLOR_F ToD2D(const Theme::Color& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

void FormatRate(double bps, wchar_t* out, size_t n) {
    if (bps < 0) {
        wcscpy_s(out, n, L"—");
        return;
    }
    if (bps >= 1e9) {
        swprintf_s(out, n, L"%.1f GB/s", bps / 1e9);
    } else if (bps >= 1e6) {
        swprintf_s(out, n, L"%.1f MB/s", bps / 1e6);
    } else if (bps >= 1e3) {
        swprintf_s(out, n, L"%.1f KB/s", bps / 1e3);
    } else {
        swprintf_s(out, n, L"%.0f B/s", bps);
    }
}

void FormatBytes(ULONG64 bytes, wchar_t* out, size_t n) {
    const double b = static_cast<double>(bytes);
    if (b >= 1e12) {
        swprintf_s(out, n, L"%.2f TB", b / 1e12);
    } else if (b >= 1e9) {
        swprintf_s(out, n, L"%.2f GB", b / 1e9);
    } else if (b >= 1e6) {
        swprintf_s(out, n, L"%.2f MB", b / 1e6);
    } else if (b >= 1e3) {
        swprintf_s(out, n, L"%.2f KB", b / 1e3);
    } else {
        swprintf_s(out, n, L"%llu B", static_cast<unsigned long long>(bytes));
    }
}

void FormatPing(double ms, wchar_t* out, size_t n) {
    if (ms < 0) {
        wcscpy_s(out, n, L"—");
        return;
    }
    swprintf_s(out, n, L"%.1f ms", ms);
}

void FormatLoss(double pct, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"%.0f%%", pct);
}

void FormatMbps(double mbps, wchar_t* out, size_t n) {
    if (mbps <= 0.0) {
        wcscpy_s(out, n, L"—");
        return;
    }
    swprintf_s(out, n, L"%.0f Mbps", mbps);
}

}  // namespace

PopupWindow::PopupWindow() = default;

PopupWindow::~PopupWindow() {
    Destroy();
}

bool PopupWindow::Create(HINSTANCE instance, HWND owner) {
    instance_ = instance;
    owner_ = owner;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    if (FAILED(hr)) {
        return false;
    }
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (FAILED(hr)) {
        return false;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kPopupClass,
        L"Latenci",
        WS_POPUP,
        0, 0,
        static_cast<int>(Theme::kPanelWidth),
        static_cast<int>(Theme::kPanelHeight),
        owner_,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }

    // Opaque paper panel — no acrylic / glass backdrop.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    ApplyPaperChrome();
    Layout();
    RebuildVisual();
    return true;
}

void PopupWindow::Destroy() {
    const bool wasVisible = IsVisible();
    DiscardDeviceResources();
    if (fmtTitle_) { fmtTitle_->Release(); fmtTitle_ = nullptr; }
    if (fmtBrand_) { fmtBrand_->Release(); fmtBrand_ = nullptr; }
    if (fmtLabel_) { fmtLabel_->Release(); fmtLabel_ = nullptr; }
    if (fmtValue_) { fmtValue_->Release(); fmtValue_ = nullptr; }
    if (fmtSection_) { fmtSection_->Release(); fmtSection_ = nullptr; }
    if (fmtPill_) { fmtPill_->Release(); fmtPill_ = nullptr; }
    if (fmtBadge_) { fmtBadge_->Release(); fmtBadge_ = nullptr; }
    if (dwriteFactory_) { dwriteFactory_->Release(); dwriteFactory_ = nullptr; }
    if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (wasVisible && onVisibility_) {
        onVisibility_(false);
    }
}

bool PopupWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

float PopupWindow::ToDips(int clientPixels) const {
    return static_cast<float>(clientPixels) * 96.0f / static_cast<float>(dpi_);
}

void PopupWindow::ApplyPaperChrome() {
    if (!hwnd_) {
        return;
    }

    // Soft rounded corners without Mica / acrylic glass.
    const DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));

    const DWORD backdrop = 1; // DWMSBT_NONE
    DwmSetWindowAttribute(hwnd_, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop));

    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    // Cream caption-bar tint so any residual DWM chrome matches the panel.
    const COLORREF caption = RGB(0xF7, 0xF0, 0xE2);
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    const COLORREF border = RGB(0xA7, 0xA2, 0x8B);
    DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

void PopupWindow::ShowNearTray(const POINT& anchorScreen, ShowMode mode) {
    if (!hwnd_) {
        return;
    }

    showMode_ = mode;

    const float scale = static_cast<float>(dpi_) / 96.0f;
    const int w = static_cast<int>(Theme::kPanelWidth * scale + 0.5f);
    const int h = static_cast<int>(Theme::kPanelHeight * scale + 0.5f);

    HMONITOR mon = MonitorFromPoint(anchorScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;

    int x = anchorScreen.x - w / 2;
    int y = anchorScreen.y - h - 12;

    if (x + w > wa.right - 8) {
        x = wa.right - w - 8;
    }
    if (x < wa.left + 8) {
        x = wa.left + 8;
    }
    if (y < wa.top + 8) {
        y = anchorScreen.y + 12;
    }
    if (y + h > wa.bottom - 8) {
        y = wa.bottom - h - 8;
    }

    const UINT flags = (mode == ShowMode::Pinned)
        ? (SWP_SHOWWINDOW)
        : (SWP_SHOWWINDOW | SWP_NOACTIVATE);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, flags);
    if (mode == ShowMode::Pinned) {
        SetForegroundWindow(hwnd_);
    }
    Layout();
    RebuildVisual();
    ForceRepaint();
    if (onVisibility_) {
        onVisibility_(true);
    }
}

void PopupWindow::SetPinned() {
    if (!IsVisible()) {
        return;
    }
    showMode_ = ShowMode::Pinned;
    SetForegroundWindow(hwnd_);
    Layout();
    RefreshAndInvalidate();
}

void PopupWindow::Hide() {
    const bool wasVisible = IsVisible();
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    showMode_ = ShowMode::Hover;
    panelView_ = PanelView::Main;
    hoverHit_ = PopupHit::None;
    if (wasVisible && onVisibility_) {
        onVisibility_(false);
    }
}

void PopupWindow::OpenSettings() {
    panelView_ = PanelView::Settings;
    Layout();
    RefreshAndInvalidate();
}

void PopupWindow::ShowMain() {
    panelView_ = PanelView::Main;
    Layout();
    RefreshAndInvalidate();
}

bool PopupWindow::ContainsScreenPoint(POINT screenPt) const {
    if (!IsVisible()) {
        return false;
    }
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    return PtInRect(&rc, screenPt) != FALSE;
}

void PopupWindow::RebuildVisual() {
    VisualState v{};
    if (panelView_ == PanelView::Settings) {
        wcsncpy_s(v.title, L"Settings", _TRUNCATE);
        wcsncpy_s(v.connection, L"Latenci", _TRUNCATE);
    } else {
        wcsncpy_s(v.title, L"Latenci", _TRUNCATE);
        wcsncpy_s(v.connection, snap_.adapterName[0] ? snap_.adapterName : L"Ethernet", _TRUNCATE);
    }
    wcsncpy_s(v.linkSpeed, snap_.linkSpeedLabel[0] ? snap_.linkSpeedLabel : L"—", _TRUNCATE);
    FormatPing(snap_.pingMs, v.ping, 32);
    FormatLoss(snap_.packetLossPct, v.loss, 32);
    FormatRate(snap_.recvBps, v.recv, 32);
    FormatRate(snap_.sendBps, v.send, 32);
    FormatBytes(snap_.downloadedBytes, v.downloaded, 32);
    FormatBytes(snap_.uploadedBytes, v.uploaded, 32);
    FormatMbps(snap_.downloadMbps, v.downloadMbps, 32);
    FormatMbps(snap_.uploadMbps, v.uploadMbps, 32);
    wcsncpy_s(v.ipAddress, snap_.ipAddress[0] ? snap_.ipAddress : L"—", _TRUNCATE);
    wcsncpy_s(v.frequency, snap_.frequency[0] ? snap_.frequency : L"—", _TRUNCATE);
    wcsncpy_s(v.updateVersion, updateVersion_, _TRUNCATE);
    wcsncpy_s(v.updateStatus, updateStatus_, _TRUNCATE);
    swprintf_s(v.appVersion, L"v%hs", APP_VERSION);
    v.mode = showMode_;
    v.view = panelView_;
    v.dns = dnsProvider_;
    v.hover = hoverHit_;
    v.speedRunning = speedRunning_;
    v.autostart = autostartEnabled_;
    v.updateAvailable = updateAvailable_;
    v.updateBusy = updateBusy_;
    memcpy(&visual_, &v, sizeof(VisualState));
}

void PopupWindow::RefreshAndInvalidate() {
    RebuildVisual();
    if (!IsVisible()) {
        return;
    }
    // Nothing the user can see moved — skip the repaint entirely.
    if (memcmp(&visual_, &painted_, sizeof(VisualState)) == 0) {
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PopupWindow::ForceRepaint() {
    memset(&painted_, 0, sizeof(painted_));
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PopupWindow::SetSnapshot(const NetworkSnapshot& snap) {
    // Static labels only change when the monitor bumps staticGeneration.
    if (snap.staticGeneration != snap_.staticGeneration) {
        snap_.adapterValid = snap.adapterValid;
        snap_.ifIndex = snap.ifIndex;
        snap_.luid = snap.luid;
        snap_.interfaceGuid = snap.interfaceGuid;
        wcsncpy_s(snap_.adapterName, snap.adapterName, _TRUNCATE);
        wcsncpy_s(snap_.ipAddress, snap.ipAddress, _TRUNCATE);
        wcsncpy_s(snap_.frequency, snap.frequency, _TRUNCATE);
        wcsncpy_s(snap_.linkSpeedLabel, snap.linkSpeedLabel, _TRUNCATE);
        snap_.staticGeneration = snap.staticGeneration;
    }

    snap_.connected = snap.connected;
    snap_.pingMs = snap.pingMs;
    snap_.packetLossPct = snap.packetLossPct;
    snap_.recvBps = snap.recvBps;
    snap_.sendBps = snap.sendBps;
    snap_.downloadedBytes = snap.downloadedBytes;
    snap_.uploadedBytes = snap.uploadedBytes;
    snap_.downloadMbps = snap.downloadMbps;
    snap_.uploadMbps = snap.uploadMbps;
    snap_.speedTestRunning = snap.speedTestRunning;
    speedRunning_ = snap.speedTestRunning;

    RefreshAndInvalidate();
}

void PopupWindow::SetDnsProvider(DnsProvider provider) {
    dnsProvider_ = provider;
    RefreshAndInvalidate();
}

void PopupWindow::SetSpeedTestRunning(bool running) {
    speedRunning_ = running;
    RefreshAndInvalidate();
}

void PopupWindow::SetAutostartEnabled(bool enabled) {
    autostartEnabled_ = enabled;
    RefreshAndInvalidate();
}

void PopupWindow::SetUpdateAvailable(bool available, const wchar_t* version) {
    updateAvailable_ = available;
    if (version && version[0]) {
        wcsncpy_s(updateVersion_, version, _TRUNCATE);
    } else {
        updateVersion_[0] = L'\0';
    }
    RefreshAndInvalidate();
}

void PopupWindow::SetUpdateStatus(const wchar_t* status) {
    if (status) {
        wcsncpy_s(updateStatus_, status, _TRUNCATE);
    } else {
        updateStatus_[0] = L'\0';
    }
    RefreshAndInvalidate();
}

void PopupWindow::SetUpdateBusy(bool busy) {
    updateBusy_ = busy;
    RefreshAndInvalidate();
}

void PopupWindow::Layout() {
    const float pad = Theme::kPadding;
    const float w = Theme::kPanelWidth;
    const float badge = Theme::kIconBadgeSize;

    iconBtn_ = D2D1::RectF(pad, pad, pad + badge, pad + badge);

    {
        const float cx1 = w - pad;
        const float cx0 = cx1 - Theme::kCloseSize;
        const float cy0 = pad + (badge - Theme::kCloseSize) * 0.5f;
        const float cy1 = cy0 + Theme::kCloseSize;
        const float hit = Theme::kCloseHitPad;
        closeBtn_ = D2D1::RectF(cx0 - hit, cy0 - hit, cx1 + hit, cy1 + hit);
    }

    runBtn_ = {};
    for (int i = 0; i < 4; ++i) {
        dnsBtns_[i] = {};
    }
    startupBtn_ = {};
    checkUpdateBtn_ = {};
    downloadUpdateBtn_ = {};

    const float contentLeft = pad;
    const float contentRight = w - pad;
    const float contentW = contentRight - contentLeft;
    float cursor = pad + badge + Theme::kSectionGap;

    if (panelView_ == PanelView::Settings) {
        // Version row
        cursor += Theme::kLabelSize + Theme::kRowGap;
        // Startup toggle
        startupBtn_ = D2D1::RectF(contentRight - 72.0f, cursor,
                                  contentRight, cursor + Theme::kSegButtonH);
        cursor += Theme::kSegButtonH + Theme::kRowGap + 2.0f + Theme::kSectionGap;
        // Updates section title + status + buttons
        cursor += Theme::kSectionSize + Theme::kLabelFieldGap + Theme::kRowGap;
        cursor += Theme::kLabelSize + Theme::kRowGap;
        checkUpdateBtn_ = D2D1::RectF(contentLeft, cursor,
                                      contentLeft + 200.0f, cursor + Theme::kRunButtonH);
        downloadUpdateBtn_ = D2D1::RectF(contentLeft + 212.0f, cursor,
                                         contentRight, cursor + Theme::kRunButtonH);
        return;
    }

    cursor += 4 * (Theme::kLabelSize + Theme::kRowGap);
    cursor += 2.0f + Theme::kSectionGap;
    runBtn_ = D2D1::RectF(
        contentRight - Theme::kRunButtonW,
        cursor,
        contentRight,
        cursor + Theme::kRunButtonH);
    cursor += Theme::kRunButtonH + Theme::kRowGap;
    cursor += Theme::kLabelSize + Theme::kRowGap;
    cursor += 2.0f + Theme::kSectionGap;
    cursor += Theme::kSectionSize + Theme::kLabelFieldGap + Theme::kRowGap;

    const float segW = contentW / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float x0 = contentLeft + i * segW;
        dnsBtns_[i] = D2D1::RectF(x0, cursor, x0 + segW, cursor + Theme::kSegButtonH);
    }
}

PopupHit PopupWindow::HitTest(float x, float y) const {
    auto contains = [](const D2D1_RECT_F& r, float px, float py) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };
    if (showMode_ == ShowMode::Pinned && contains(closeBtn_, x, y)) {
        return PopupHit::Close;
    }
    if (contains(iconBtn_, x, y)) {
        return PopupHit::IconBadge;
    }
    if (panelView_ == PanelView::Settings) {
        if (contains(startupBtn_, x, y)) {
            return PopupHit::SettingsStartup;
        }
        if (contains(checkUpdateBtn_, x, y)) {
            return PopupHit::SettingsCheckUpdate;
        }
        if (updateAvailable_ && contains(downloadUpdateBtn_, x, y)) {
            return PopupHit::SettingsDownload;
        }
        return PopupHit::None;
    }
    if (contains(runBtn_, x, y)) {
        return PopupHit::RunSpeedTest;
    }
    if (contains(dnsBtns_[0], x, y)) return PopupHit::DnsDhcp;
    if (contains(dnsBtns_[1], x, y)) return PopupHit::DnsCloudflare;
    if (contains(dnsBtns_[2], x, y)) return PopupHit::DnsGoogle;
    if (contains(dnsBtns_[3], x, y)) return PopupHit::DnsCustom;
    return PopupHit::None;
}

bool PopupWindow::IsInteractiveHit(PopupHit hit) const {
    switch (hit) {
    case PopupHit::Close:
    case PopupHit::IconBadge:
    case PopupHit::RunSpeedTest:
    case PopupHit::DnsDhcp:
    case PopupHit::DnsCloudflare:
    case PopupHit::DnsGoogle:
    case PopupHit::DnsCustom:
    case PopupHit::SettingsStartup:
    case PopupHit::SettingsCheckUpdate:
    case PopupHit::SettingsDownload:
        return true;
    case PopupHit::None:
        return false;
    }
    return false;
}

void PopupWindow::BeginDrag() {
    if (!hwnd_ || showMode_ != ShowMode::Pinned) {
        return;
    }
    ReleaseCapture();
    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

IDWriteTextFormat* PopupWindow::Format(float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* fmt = nullptr;
    IDWriteFontCollection* collection = GetAppFontCollection();
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        Theme::kFontFamily,
        collection,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size,
        L"en-us",
        &fmt);
    if (FAILED(hr) || !fmt) {
        dwriteFactory_->CreateTextFormat(
            Theme::kFontFallback,
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            &fmt);
    }
    return fmt;
}

float PopupWindow::MeasureText(const wchar_t* text, IDWriteTextFormat* fmt, float maxW) const {
    if (!fmt || !dwriteFactory_) {
        return 0.0f;
    }
    IDWriteTextLayout* layout = nullptr;
    float textW = 0.0f;
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text, static_cast<UINT32>(wcslen(text)), fmt, maxW, 40.0f, &layout)) && layout) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        textW = m.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    return textW;
}

void PopupWindow::RefreshLabelMetrics() {
    if (haveLabelMetrics_) {
        return;
    }
    const float w = Theme::kPanelWidth;
    sectionSpeedTestW_ = MeasureText(kSectionSpeedTest, fmtSection_, w * 0.55f - Theme::kPadding);
    sectionDnsW_ = MeasureText(kSectionDns, fmtSection_, w - 2 * Theme::kPadding);
    haveLabelMetrics_ = true;
}

bool PopupWindow::EnsureDeviceResources() {
    if (renderTarget_) {
        return true;
    }
    if (!hwnd_ || !d2dFactory_) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    // Report the window DPI so layout stays in 440x346 DIPs at any scale.
    const float dpi = static_cast<float>(dpi_);
    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            dpi, dpi),
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        &renderTarget_);
    if (FAILED(hr)) {
        return false;
    }

    if (!fmtTitle_) {
        fmtTitle_ = Format(Theme::kTitleSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtBrand_ = Format(Theme::kBrandSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtLabel_ = Format(Theme::kLabelSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtValue_ = Format(Theme::kValueSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtSection_ = Format(Theme::kSectionSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtPill_ = Format(Theme::kPillSize, DWRITE_FONT_WEIGHT_NORMAL);
        fmtBadge_ = Format(10.0f, DWRITE_FONT_WEIGHT_NORMAL);

        if (fmtTitle_) fmtTitle_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fmtBrand_) {
            fmtBrand_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmtBrand_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (fmtLabel_) fmtLabel_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fmtValue_) fmtValue_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (fmtSection_) fmtSection_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (fmtPill_) {
            fmtPill_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtPill_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (fmtBadge_) {
            fmtBadge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmtBadge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    RefreshLabelMetrics();
    return true;
}

void PopupWindow::ReleaseBrushes() {
    auto release = [](ID2D1SolidColorBrush*& b) {
        if (b) { b->Release(); b = nullptr; }
    };
    release(brushPanel_);
    release(brushBorder_);
    release(brushInk_);
    release(brushMuted_);
    release(brushDim_);
    release(brushHair_);
    release(brushLabel_);
    release(brushSubtitle_);
    release(brushSurface_);
    release(brushSurfaceBorder_);
    release(brushStipple_);
    release(brushStippleBtn_);
    release(brushAccentHover_);
    release(brushIconBg_);
    release(brushIconFg_);
    release(brushGrain_);
    release(brushLeaf_);
    release(brushGold_);
    release(brushOnGold_);
}

bool PopupWindow::EnsureBrushes(ID2D1RenderTarget* rt) {
    if (brushPanel_) {
        return true;
    }
    auto make = [&](const Theme::Color& c, ID2D1SolidColorBrush** out) {
        return SUCCEEDED(rt->CreateSolidColorBrush(ToD2D(c), out)) && *out;
    };
    if (!make(Theme::kPanel, &brushPanel_) ||
        !make(Theme::kPanelBorder, &brushBorder_) ||
        !make(Theme::kInk, &brushInk_) ||
        !make(Theme::kInkMuted, &brushMuted_) ||
        !make(Theme::kTextDim, &brushDim_) ||
        !make(Theme::kHairline, &brushHair_) ||
        !make(Theme::kLabel, &brushLabel_) ||
        !make(Theme::kSubtitle, &brushSubtitle_) ||
        !make(Theme::kSurface, &brushSurface_) ||
        !make(Theme::kSurfaceBorder, &brushSurfaceBorder_) ||
        !make(Theme::kStipple, &brushStipple_) ||
        !make(Theme::kStippleBtn, &brushStippleBtn_) ||
        !make(Theme::kAccentHover, &brushAccentHover_) ||
        !make(Theme::kIconBadgeBg, &brushIconBg_) ||
        !make(Theme::kIconBadgeFg, &brushIconFg_) ||
        !make(Theme::kGrain, &brushGrain_) ||
        !make(Theme::kLeaf, &brushLeaf_) ||
        !make(Theme::kGold, &brushGold_) ||
        !make(Theme::kOnGold, &brushOnGold_)) {
        ReleaseBrushes();
        return false;
    }
    return true;
}

bool PopupWindow::EnsureGrainBrush(ID2D1RenderTarget* rt) {
    if (grainBrush_) {
        return true;
    }
    if (!brushGrain_) {
        return false;
    }
    D2D1_SIZE_U pixelSize = D2D1::SizeU(kGrainTile, kGrainTile);
    D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ID2D1BitmapRenderTarget* layer = nullptr;
    if (FAILED(rt->CreateCompatibleRenderTarget(
            D2D1::SizeF(static_cast<float>(kGrainTile), static_cast<float>(kGrainTile)),
            pixelSize, pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &layer)) || !layer) {
        return false;
    }
    layer->BeginDraw();
    layer->Clear(D2D1::ColorF(0, 0, 0, 0));
    auto hash = [](unsigned n) -> unsigned {
        n ^= n << 13;
        n ^= n >> 17;
        n ^= n << 5;
        return n;
    };
    for (unsigned i = 0; i < kGrainDots; ++i) {
        const unsigned hx = hash(i * 2654435761u + 101);
        const unsigned hy = hash(i * 1597334677u + 77);
        const float gx = static_cast<float>(hx % kGrainTile);
        const float gy = static_cast<float>(hy % kGrainTile);
        layer->FillRectangle(D2D1::RectF(gx, gy, gx + 1.0f, gy + 1.0f), brushGrain_);
    }
    if (FAILED(layer->EndDraw())) {
        layer->Release();
        return false;
    }
    ID2D1Bitmap* tile = nullptr;
    HRESULT hr = layer->GetBitmap(&tile);
    layer->Release();
    if (FAILED(hr) || !tile) {
        return false;
    }
    hr = rt->CreateBitmapBrush(
        tile,
        D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP,
            D2D1_EXTEND_MODE_WRAP,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
        &grainBrush_);
    tile->Release();
    return SUCCEEDED(hr) && grainBrush_;
}

void PopupWindow::DiscardDeviceResources() {
    if (grainBrush_) {
        grainBrush_->Release();
        grainBrush_ = nullptr;
    }
    ReleaseBrushes();
    if (renderTarget_) {
        renderTarget_->Release();
        renderTarget_ = nullptr;
    }
    haveLabelMetrics_ = false;
    speedPillMeasuredFor_[0] = L'\0';
    speedPillTextW_ = 0.0f;
}

void PopupWindow::Paint() {
    if (!EnsureDeviceResources()) {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->Clear(ToD2D(Theme::kPageBg));
    DrawPanel(renderTarget_);

    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        ForceRepaint();
        return;
    }
    memcpy(&painted_, &visual_, sizeof(VisualState));
}

void PopupWindow::DrawPanel(ID2D1RenderTarget* rt) {
    if (!EnsureBrushes(rt)) {
        return;
    }
    EnsureGrainBrush(rt);

    const VisualState& v = visual_;
    const float w = Theme::kPanelWidth;
    const float h = Theme::kPanelHeight;
    const float r = Theme::kCornerRadius;
    const float pad = Theme::kPadding;

    D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r, r);
    rt->FillRoundedRectangle(panel, brushPanel_);
    rt->DrawRoundedRectangle(panel, brushBorder_, Theme::kBorderWidth);

    if (grainBrush_) {
        rt->FillRectangle(D2D1::RectF(0, 0, w, h), grainBrush_);
    }

    auto drawText = [&](const wchar_t* text, const D2D1_RECT_F& rc,
                        IDWriteTextFormat* fmt, ID2D1Brush* br) {
        if (!fmt || !br) return;
        rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt, rc, br,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    };

    // --- Header ---
    const float badge = Theme::kIconBadgeSize;
    iconBtn_ = D2D1::RectF(pad, pad, pad + badge, pad + badge);
    const bool iconHot = (v.hover == PopupHit::IconBadge);
    const D2D1_ROUNDED_RECT badgeRc = D2D1::RoundedRect(iconBtn_, Theme::kIconBadgeRadius, Theme::kIconBadgeRadius);
    rt->FillRoundedRectangle(badgeRc, iconHot ? brushAccentHover_ : brushIconBg_);
    rt->DrawRoundedRectangle(badgeRc, brushIconFg_, iconHot ? 1.5f : 1.0f);

    // Soft pixel ethernet glyph (blocky silhouette)
    {
        const float bx = pad + 6.0f;
        const float by = pad + 7.0f;
        auto px = [&](float x, float y, float s = 2.0f) {
            rt->FillRectangle(D2D1::RectF(bx + x, by + y, bx + x + s, by + y + s), brushIconFg_);
        };
        for (int x = 0; x <= 7; ++x) {
            px(static_cast<float>(x) * 2.0f, 0.0f);
            px(static_cast<float>(x) * 2.0f, 10.0f);
        }
        for (int y = 1; y <= 4; ++y) {
            px(0.0f, static_cast<float>(y) * 2.0f);
            px(14.0f, static_cast<float>(y) * 2.0f);
        }
        px(4.0f, 12.0f);
        px(6.0f, 12.0f);
        px(8.0f, 12.0f);
        px(10.0f, 12.0f);
        if (brushLeaf_) {
            rt->FillRectangle(D2D1::RectF(bx + 4.0f, by + 4.0f, bx + 6.0f, by + 6.0f), brushLeaf_);
            rt->FillRectangle(D2D1::RectF(bx + 8.0f, by + 6.0f, bx + 10.0f, by + 8.0f), brushLeaf_);
        }
    }

    // Golden "i" update badge on the icon's top-left corner.
    if (v.updateAvailable && brushGold_ && brushOnGold_ && fmtBadge_) {
        const float dot = Theme::kUpdateDotSize;
        const float dx0 = pad - 3.0f;
        const float dy0 = pad - 3.0f;
        const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(dx0 + dot * 0.5f, dy0 + dot * 0.5f),
                                              dot * 0.5f, dot * 0.5f);
        rt->FillEllipse(el, brushGold_);
        rt->DrawEllipse(el, brushInk_, 1.0f);
        drawText(L"i", D2D1::RectF(dx0, dy0 - 0.5f, dx0 + dot, dy0 + dot), fmtBadge_, brushOnGold_);
    }

    const float titleRightPad = (v.mode == ShowMode::Pinned)
        ? (Theme::kCloseSize + Theme::kCloseHitPad * 2.0f + 100.0f)
        : 90.0f;
    drawText(v.title,
             D2D1::RectF(pad + badge + 10.0f, pad - 1.0f, w - pad - titleRightPad, pad + 22.0f),
             fmtTitle_, brushInk_);
    drawText(v.connection,
             D2D1::RectF(pad + badge + 10.0f, pad + 20.0f, w - pad - titleRightPad, pad + 36.0f),
             fmtBrand_, brushDim_);

    float speedPillRight = w - pad;
    if (v.mode == ShowMode::Pinned) {
        const float cx1 = w - pad;
        const float cx0 = cx1 - Theme::kCloseSize;
        const float cy0 = pad + (Theme::kIconBadgeSize - Theme::kCloseSize) * 0.5f;
        const float cy1 = cy0 + Theme::kCloseSize;
        closeBtn_ = D2D1::RectF(
            cx0 - Theme::kCloseHitPad,
            cy0 - Theme::kCloseHitPad,
            cx1 + Theme::kCloseHitPad,
            cy1 + Theme::kCloseHitPad);

        ID2D1SolidColorBrush* brushClose = brushMuted_;
        if (v.hover == PopupHit::Close) {
            brushClose = brushInk_;
        }
        rt->DrawLine(D2D1::Point2F(cx0, cy0), D2D1::Point2F(cx1, cy1),
                     brushClose, Theme::kCloseStroke);
        rt->DrawLine(D2D1::Point2F(cx1, cy0), D2D1::Point2F(cx0, cy1),
                     brushClose, Theme::kCloseStroke);

        speedPillRight = cx0 - 10.0f;
    }

    if (v.view == PanelView::Main) {
        auto drawChunkyButton = [&](const D2D1_RECT_F& rc, bool primary, bool hot,
                                    const wchar_t* label) {
            const float rad = Theme::kControlRadius;
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, rad, rad);
            ID2D1SolidColorBrush* fill = primary
                ? (hot ? brushAccentHover_ : brushStippleBtn_)
                : brushSurface_;
            ID2D1SolidColorBrush* edge = primary ? brushInk_ : brushSurfaceBorder_;
            const float bottomW = primary ? Theme::kBtnBorderBottom
                                          : (hot ? Theme::kBtnBorderBottom : Theme::kSecondaryBottom);
            rt->FillRoundedRectangle(rr, fill);
            rt->DrawRoundedRectangle(rr, edge, Theme::kBtnBorderSide);
            rt->DrawLine(
                D2D1::Point2F(rc.left + rad, rc.bottom - 0.5f),
                D2D1::Point2F(rc.right - rad, rc.bottom - 0.5f),
                edge, bottomW);
            drawText(label, rc, fmtPill_, brushInk_);
        };

        if (wcscmp(speedPillMeasuredFor_, v.linkSpeed) != 0) {
            speedPillTextW_ = MeasureText(v.linkSpeed, fmtPill_, 200.0f);
            wcsncpy_s(speedPillMeasuredFor_, v.linkSpeed, _TRUNCATE);
        }
        const float textW = speedPillTextW_ < 1.0f ? 48.0f : speedPillTextW_;
        const float pillW = textW + Theme::kSpeedPillPadX * 2.0f;
        const float px1 = speedPillRight;
        const float px0 = px1 - pillW;
        const float py0 = pad + (badge - Theme::kSpeedPillH) * 0.5f;
        const float py1 = py0 + Theme::kSpeedPillH;
        drawChunkyButton(D2D1::RectF(px0, py0, px1, py1), false, false, v.linkSpeed);
    }

    float y = pad + badge + Theme::kSectionGap;
    if (v.view == PanelView::Settings) {
        DrawSettingsBody(rt, v, y);
    } else {
        DrawMainBody(rt, v, y);
    }

    if (brushLeaf_) {
        const float lx = w - pad - 18.0f;
        const float ly = h - pad - 16.0f;
        auto leafPx = [&](float x, float y) {
            rt->FillRectangle(D2D1::RectF(lx + x, ly + y, lx + x + 2.0f, ly + y + 2.0f), brushLeaf_);
        };
        leafPx(8.0f, 14.0f);
        leafPx(8.0f, 12.0f);
        leafPx(8.0f, 10.0f);
        leafPx(8.0f, 8.0f);
        leafPx(4.0f, 6.0f);
        leafPx(2.0f, 4.0f);
        leafPx(4.0f, 4.0f);
        leafPx(6.0f, 6.0f);
        leafPx(10.0f, 6.0f);
        leafPx(12.0f, 4.0f);
        leafPx(14.0f, 4.0f);
        leafPx(12.0f, 6.0f);
        leafPx(6.0f, 2.0f);
        leafPx(10.0f, 2.0f);
    }
}

void PopupWindow::DrawMainBody(ID2D1RenderTarget* rt, const VisualState& v, float& y) {
    const float w = Theme::kPanelWidth;
    const float pad = Theme::kPadding;

    auto drawText = [&](const wchar_t* text, const D2D1_RECT_F& rc,
                        IDWriteTextFormat* fmt, ID2D1Brush* br) {
        if (!fmt || !br) return;
        rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt, rc, br,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    };
    auto drawSectionTitle = [&](const wchar_t* text, float textW, float x, float y0, float maxRight) {
        const float titleH = Theme::kSectionSize + 2.0f;
        drawText(text, D2D1::RectF(x, y0, maxRight, y0 + titleH), fmtSection_, brushSubtitle_);
        const float uy = y0 + Theme::kSectionSize + 1.0f;
        rt->DrawLine(D2D1::Point2F(x, uy), D2D1::Point2F(x + textW, uy), brushSubtitle_, 1.0f);
    };
    auto drawChunkyButton = [&](const D2D1_RECT_F& rc, bool primary, bool hot,
                                const wchar_t* label) {
        const float rad = Theme::kControlRadius;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, rad, rad);
        ID2D1SolidColorBrush* fill = primary
            ? (hot ? brushAccentHover_ : brushStippleBtn_)
            : brushSurface_;
        ID2D1SolidColorBrush* edge = primary ? brushInk_ : brushSurfaceBorder_;
        const float bottomW = primary ? Theme::kBtnBorderBottom
                                      : (hot ? Theme::kBtnBorderBottom : Theme::kSecondaryBottom);
        rt->FillRoundedRectangle(rr, fill);
        rt->DrawRoundedRectangle(rr, edge, Theme::kBtnBorderSide);
        rt->DrawLine(
            D2D1::Point2F(rc.left + rad, rc.bottom - 0.5f),
            D2D1::Point2F(rc.right - rad, rc.bottom - 0.5f),
            edge, bottomW);
        drawText(label, rc, fmtPill_, brushInk_);
    };

    auto drawStatPair = [&](const wchar_t* l1, const wchar_t* v1,
                            const wchar_t* l2, const wchar_t* v2) {
        const float mid = pad + (w - 2 * pad) * 0.5f + 6.0f;
        const float colW = (w - 2 * pad) * 0.5f - 8.0f;
        const float rowH = Theme::kLabelSize + 4.0f;
        const float gap = Theme::kLabelFieldGap + 2.0f;

        auto drawCol = [&](float x0, float x1, const wchar_t* label, const wchar_t* value) {
            float labelW = MeasureText(label, fmtLabel_, colW);
            // Keep value column usable even for long labels (e.g. "IP Address").
            const float maxLabelW = colW * 0.58f;
            if (labelW > maxLabelW) {
                labelW = maxLabelW;
            }
            const float split = x0 + labelW + gap;
            drawText(label, D2D1::RectF(x0, y, split, y + rowH), fmtLabel_, brushLabel_);
            drawText(value, D2D1::RectF(split, y, x1, y + rowH), fmtValue_, brushInk_);
        };
        drawCol(pad, pad + colW, l1, v1);
        drawCol(mid, w - pad, l2, v2);
        y += Theme::kLabelSize + Theme::kRowGap;
    };

    drawStatPair(L"Ping", v.ping, L"Packet Loss", v.loss);
    drawStatPair(L"Receiving", v.recv, L"Sending", v.send);
    drawStatPair(L"Downloaded", v.downloaded, L"Uploaded", v.uploaded);
    drawStatPair(L"IP Address", v.ipAddress, L"Frequency", v.frequency);

    y += 2.0f;
    rt->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(w - pad, y), brushHair_, 1.0f);
    y += Theme::kSectionGap;

    drawSectionTitle(kSectionSpeedTest, sectionSpeedTestW_, pad, y + 6.0f, w * 0.55f);
    runBtn_ = D2D1::RectF(w - pad - Theme::kRunButtonW, y,
                          w - pad, y + Theme::kRunButtonH);
    {
        const bool hot = (v.hover == PopupHit::RunSpeedTest);
        const wchar_t* label = v.speedRunning ? L"..." : L"Run";
        drawChunkyButton(runBtn_, true, hot, label);
    }
    y += Theme::kRunButtonH + Theme::kRowGap;

    {
        const float mid = pad + (w - 2 * pad) * 0.5f + 6.0f;
        const float colW = (w - 2 * pad) * 0.5f - 8.0f;
        const float rowH = Theme::kLabelSize + 4.0f;
        const float gap = Theme::kLabelFieldGap + 2.0f;

        auto drawCol = [&](float x0, float x1, const wchar_t* label, const wchar_t* value) {
            float labelW = MeasureText(label, fmtLabel_, colW);
            const float maxLabelW = colW * 0.58f;
            if (labelW > maxLabelW) {
                labelW = maxLabelW;
            }
            const float split = x0 + labelW + gap;
            drawText(label, D2D1::RectF(x0, y, split, y + rowH), fmtLabel_, brushLabel_);
            drawText(value, D2D1::RectF(split, y, x1, y + rowH), fmtValue_, brushInk_);
        };
        drawCol(pad, pad + colW, L"Download", v.downloadMbps);
        drawCol(mid, w - pad, L"Upload", v.uploadMbps);
        y += Theme::kLabelSize + Theme::kRowGap;
    }

    y += 2.0f;
    rt->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(w - pad, y), brushHair_, 1.0f);
    y += Theme::kSectionGap;

    drawSectionTitle(kSectionDns, sectionDnsW_, pad, y, w - pad);
    y += Theme::kSectionSize + Theme::kLabelFieldGap + Theme::kRowGap;

    const wchar_t* dnsLabels[4] = {L"DHCP", L"Cloudflare", L"Google", L"Custom"};
    const DnsProvider dnsValues[4] = {
        DnsProvider::Dhcp, DnsProvider::Cloudflare, DnsProvider::Google, DnsProvider::Custom};
    const float contentW = w - 2 * pad;
    const float segW = contentW / 4.0f;

    D2D1_RECT_F group = D2D1::RectF(pad, y, w - pad, y + Theme::kSegButtonH);
    rt->FillRectangle(group, brushSurface_);
    rt->DrawRectangle(group, brushSurfaceBorder_, 1.0f);

    for (int i = 0; i < 4; ++i) {
        const float x0 = pad + i * segW;
        dnsBtns_[i] = D2D1::RectF(x0, y, x0 + segW, y + Theme::kSegButtonH);
        const bool selected = (v.dns == dnsValues[i]);
        const bool hot = (v.hover == static_cast<PopupHit>(
            static_cast<int>(PopupHit::DnsDhcp) + i));

        if (selected) {
            rt->FillRectangle(dnsBtns_[i], brushStipple_);
            rt->DrawRectangle(dnsBtns_[i], brushInk_, 1.0f);
        } else if (hot) {
            rt->FillRectangle(dnsBtns_[i], brushStipple_);
        }
        if (i > 0) {
            rt->DrawLine(
                D2D1::Point2F(x0, y + 3.0f),
                D2D1::Point2F(x0, y + Theme::kSegButtonH - 3.0f),
                brushHair_, 1.0f);
        }
        drawText(dnsLabels[i], dnsBtns_[i], fmtPill_, brushInk_);
    }
}

void PopupWindow::DrawSettingsBody(ID2D1RenderTarget* rt, const VisualState& v, float& y) {
    const float w = Theme::kPanelWidth;
    const float pad = Theme::kPadding;

    auto drawText = [&](const wchar_t* text, const D2D1_RECT_F& rc,
                        IDWriteTextFormat* fmt, ID2D1Brush* br) {
        if (!fmt || !br) return;
        rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt, rc, br,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    };
    auto drawChunkyButton = [&](const D2D1_RECT_F& rc, bool primary, bool hot,
                                const wchar_t* label) {
        const float rad = Theme::kControlRadius;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rc, rad, rad);
        ID2D1SolidColorBrush* fill = primary
            ? (hot ? brushAccentHover_ : brushStippleBtn_)
            : brushSurface_;
        ID2D1SolidColorBrush* edge = primary ? brushInk_ : brushSurfaceBorder_;
        const float bottomW = primary ? Theme::kBtnBorderBottom
                                      : (hot ? Theme::kBtnBorderBottom : Theme::kSecondaryBottom);
        rt->FillRoundedRectangle(rr, fill);
        rt->DrawRoundedRectangle(rr, edge, Theme::kBtnBorderSide);
        rt->DrawLine(
            D2D1::Point2F(rc.left + rad, rc.bottom - 0.5f),
            D2D1::Point2F(rc.right - rad, rc.bottom - 0.5f),
            edge, bottomW);
        drawText(label, rc, fmtPill_, brushInk_);
    };

    const float rowH = Theme::kLabelSize + 4.0f;
    drawText(L"Version", D2D1::RectF(pad, y, pad + 120.0f, y + rowH), fmtLabel_, brushLabel_);
    drawText(v.appVersion, D2D1::RectF(pad + 120.0f, y, w - pad, y + rowH), fmtValue_, brushInk_);
    y += Theme::kLabelSize + Theme::kRowGap;

    drawText(L"Launch at startup", D2D1::RectF(pad, y + 6.0f, w - pad - 84.0f, y + 6.0f + rowH),
             fmtLabel_, brushLabel_);
    startupBtn_ = D2D1::RectF(w - pad - 72.0f, y, w - pad, y + Theme::kSegButtonH);
    {
        const bool hot = (v.hover == PopupHit::SettingsStartup);
        drawChunkyButton(startupBtn_, v.autostart, hot, v.autostart ? L"On" : L"Off");
    }
    y += Theme::kSegButtonH + Theme::kRowGap;

    y += 2.0f;
    rt->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(w - pad, y), brushHair_, 1.0f);
    y += Theme::kSectionGap;

    const float updatesTitleW = MeasureText(L"Updates", fmtSection_, w - 2 * pad);
    drawText(L"Updates", D2D1::RectF(pad, y, w - pad, y + Theme::kSectionSize + 2.0f),
             fmtSection_, brushSubtitle_);
    rt->DrawLine(D2D1::Point2F(pad, y + Theme::kSectionSize + 1.0f),
                 D2D1::Point2F(pad + updatesTitleW, y + Theme::kSectionSize + 1.0f),
                 brushSubtitle_, 1.0f);
    y += Theme::kSectionSize + Theme::kLabelFieldGap + Theme::kRowGap;

    const wchar_t* status = v.updateStatus[0] ? v.updateStatus
        : (v.updateAvailable ? L"Update available" : L"No update check yet");
    drawText(status, D2D1::RectF(pad, y, w - pad, y + rowH), fmtLabel_, brushDim_);
    y += Theme::kLabelSize + Theme::kRowGap;

    const wchar_t* checkLabel = v.updateBusy ? L"..." : L"Check for updates";
    float checkW = MeasureText(checkLabel, fmtPill_, w - 2 * pad) + 28.0f;
    if (checkW < 140.0f) {
        checkW = 140.0f;
    }
    const float maxCheckW = v.updateAvailable ? (w - 2 * pad) * 0.55f : (w - 2 * pad);
    if (checkW > maxCheckW) {
        checkW = maxCheckW;
    }
    checkUpdateBtn_ = D2D1::RectF(pad, y, pad + checkW, y + Theme::kRunButtonH);
    {
        const bool hot = (v.hover == PopupHit::SettingsCheckUpdate);
        drawChunkyButton(checkUpdateBtn_, true, hot && !v.updateBusy, checkLabel);
    }

    if (v.updateAvailable) {
        downloadUpdateBtn_ = D2D1::RectF(pad + checkW + 12.0f, y, w - pad, y + Theme::kRunButtonH);
        const bool hot = (v.hover == PopupHit::SettingsDownload);
        wchar_t label[48];
        if (v.updateBusy) {
            wcscpy_s(label, L"...");
        } else if (v.updateVersion[0]) {
            swprintf_s(label, L"Download v%s", v.updateVersion);
        } else {
            wcscpy_s(label, L"Download");
        }
        drawChunkyButton(downloadUpdateBtn_, true, hot && !v.updateBusy, label);
    } else {
        downloadUpdateBtn_ = {};
    }
}

LRESULT CALLBACK PopupWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PopupWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PopupWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PopupWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        if (dpi_ == 0) {
            dpi_ = 96;
        }
        const RECT* target = reinterpret_cast<const RECT*>(lParam);
        DiscardDeviceResources();
        if (target) {
            SetWindowPos(hwnd_, nullptr,
                         target->left, target->top,
                         target->right - target->left,
                         target->bottom - target->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ForceRepaint();
        return 0;
    }

    case WM_DISPLAYCHANGE:
    case WM_SIZE:
        DiscardDeviceResources();
        ForceRepaint();
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme)};
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);

        const float x = ToDips(GET_X_LPARAM(lParam));
        const float y = ToDips(GET_Y_LPARAM(lParam));
        const PopupHit hit = HitTest(x, y);
        if (hit != hoverHit_) {
            hoverHit_ = hit;
            RefreshAndInvalidate();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        hoverHit_ = PopupHit::None;
        RefreshAndInvalidate();
        if (showMode_ == ShowMode::Hover && owner_) {
            // Owner decides: hide only if cursor is over neither tray nor panel.
            PostMessageW(owner_, WM_APP_HIDE_POPUP, 0, 0);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const float x = ToDips(GET_X_LPARAM(lParam));
        const float y = ToDips(GET_Y_LPARAM(lParam));
        const PopupHit hit = HitTest(x, y);
        if (showMode_ == ShowMode::Pinned && !IsInteractiveHit(hit)) {
            // Drag from header band or empty chrome
            if (y <= Theme::kHeaderDragH || hit == PopupHit::None) {
                BeginDrag();
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const float x = ToDips(GET_X_LPARAM(lParam));
        const float y = ToDips(GET_Y_LPARAM(lParam));
        const PopupHit hit = HitTest(x, y);
        switch (hit) {
        case PopupHit::Close:
            Hide();
            break;
        case PopupHit::IconBadge:
            if (panelView_ == PanelView::Settings) {
                ShowMain();
            } else {
                OpenSettings();
            }
            break;
        case PopupHit::RunSpeedTest:
            if (onSpeedClick_ && !speedRunning_) {
                onSpeedClick_();
            }
            break;
        case PopupHit::DnsDhcp:
            if (onDnsClick_) onDnsClick_(DnsProvider::Dhcp);
            break;
        case PopupHit::DnsCloudflare:
            if (onDnsClick_) onDnsClick_(DnsProvider::Cloudflare);
            break;
        case PopupHit::DnsGoogle:
            if (onDnsClick_) onDnsClick_(DnsProvider::Google);
            break;
        case PopupHit::DnsCustom:
            if (onDnsClick_) onDnsClick_(DnsProvider::Custom);
            break;
        case PopupHit::SettingsStartup:
            if (onStartupToggle_) {
                onStartupToggle_(!autostartEnabled_);
            }
            break;
        case PopupHit::SettingsCheckUpdate:
            if (onCheckUpdate_ && !updateBusy_) {
                onCheckUpdate_();
            }
            break;
        case PopupHit::SettingsDownload:
            if (onDownloadUpdate_ && updateAvailable_ && !updateBusy_) {
                onDownloadUpdate_();
            }
            break;
        case PopupHit::None:
            break;
        }
        return 0;
    }

    case WM_ACTIVATE:
    case WM_KILLFOCUS:
        // Pinned stays; hover dismiss is mouse-leave driven only.
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
