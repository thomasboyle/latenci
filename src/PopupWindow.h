#pragma once

#include "Messages.h"
#include "Theme.h"

#include <d2d1.h>
#include <dwrite.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

enum class ShowMode {
    Hover,
    Pinned,
};

enum class PanelView {
    Main,
    Settings,
};

enum class PopupHit {
    None = 0,
    Close,
    IconBadge,
    RunSpeedTest,
    DnsDhcp,
    DnsCloudflare,
    DnsGoogle,
    DnsCustom,
    SettingsStartup,
    SettingsCheckUpdate,
    SettingsDownload,
};

class PopupWindow {
public:
    using DnsClickHandler = void (*)(DnsProvider);
    using SpeedClickHandler = void (*)();
    using VisibilityHandler = void (*)(bool visible);
    using StartupToggleHandler = void (*)(bool enable);
    using SimpleHandler = void (*)();

    PopupWindow();
    ~PopupWindow();

    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;

    bool Create(HINSTANCE instance, HWND owner);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const;
    bool IsPinned() const { return IsVisible() && showMode_ == ShowMode::Pinned; }
    ShowMode Mode() const { return showMode_; }
    PanelView View() const { return panelView_; }

    void ShowNearTray(const POINT& anchorScreen, ShowMode mode);
    void SetPinned();
    void Hide();
    void OpenSettings();
    void ShowMain();

    // Used by main to decide hover dismiss: true if cursor is over popup client.
    bool ContainsScreenPoint(POINT screenPt) const;

    void SetSnapshot(const NetworkSnapshot& snap);
    void SetDnsProvider(DnsProvider provider);
    void SetSpeedTestRunning(bool running);
    void SetAutostartEnabled(bool enabled);
    void SetUpdateAvailable(bool available, const wchar_t* version);
    void SetUpdateStatus(const wchar_t* status);
    void SetUpdateBusy(bool busy);

    void SetDnsClickHandler(DnsClickHandler handler) { onDnsClick_ = handler; }
    void SetSpeedClickHandler(SpeedClickHandler handler) { onSpeedClick_ = handler; }
    void SetVisibilityHandler(VisibilityHandler handler) { onVisibility_ = handler; }
    void SetStartupToggleHandler(StartupToggleHandler handler) { onStartupToggle_ = handler; }
    void SetCheckUpdateHandler(SimpleHandler handler) { onCheckUpdate_ = handler; }
    void SetDownloadUpdateHandler(SimpleHandler handler) { onDownloadUpdate_ = handler; }

private:
    struct VisualState {
        wchar_t title[128];
        wchar_t connection[128];
        wchar_t linkSpeed[32];
        wchar_t ping[32];
        wchar_t loss[32];
        wchar_t recv[32];
        wchar_t send[32];
        wchar_t downloaded[32];
        wchar_t uploaded[32];
        wchar_t downloadMbps[32];
        wchar_t uploadMbps[32];
        wchar_t ipAddress[64];
        wchar_t frequency[32];
        wchar_t updateVersion[32];
        wchar_t updateStatus[96];
        wchar_t appVersion[32];
        ShowMode mode;
        PanelView view;
        DnsProvider dns;
        PopupHit hover;
        bool speedRunning;
        bool autostart;
        bool updateAvailable;
        bool updateBusy;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    bool EnsureBrushes(ID2D1RenderTarget* rt);
    void ReleaseBrushes();
    bool EnsureGrainBrush(ID2D1RenderTarget* rt);
    void Paint();
    void DrawPanel(ID2D1RenderTarget* rt);
    void DrawMainBody(ID2D1RenderTarget* rt, const VisualState& v, float& y);
    void DrawSettingsBody(ID2D1RenderTarget* rt, const VisualState& v, float& y);
    void DrawText(ID2D1RenderTarget* rt, const wchar_t* text, const D2D1_RECT_F& rc,
                  IDWriteTextFormat* fmt, ID2D1Brush* br) const;
    void DrawChunkyButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc, bool primary,
                          bool hot, const wchar_t* label);
    void Layout();
    PopupHit HitTest(float x, float y) const;
    void ApplyPaperChrome();
    bool IsInteractiveHit(PopupHit hit) const;
    void BeginDrag();

    void RebuildVisual(VisualState& v);
    void RefreshAndInvalidate();
    void ForceRepaint();
    float MeasureText(const wchar_t* text, IDWriteTextFormat* fmt, float maxW) const;
    // Measures text once per (text, fmt, maxW) triple; the stat-pair and section
    // labels are compile-time constants, so their widths never change and the
    // per-frame CreateTextLayout allocations are wasted work.
    float CachedLabelWidth(const wchar_t* text, IDWriteTextFormat* fmt, float maxW);
    void RefreshLabelMetrics();
    bool EnsureChromeTiles(ID2D1RenderTarget* rt);
    float ToDips(int clientPixels) const;

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    ShowMode showMode_ = ShowMode::Hover;
    PanelView panelView_ = PanelView::Main;
    UINT dpi_ = 96;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;

    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtBrand_ = nullptr;
    IDWriteTextFormat* fmtLabel_ = nullptr;
    IDWriteTextFormat* fmtValue_ = nullptr;
    IDWriteTextFormat* fmtSection_ = nullptr;
    IDWriteTextFormat* fmtPill_ = nullptr;
    IDWriteTextFormat* fmtBadge_ = nullptr;

    ID2D1SolidColorBrush* brushPanel_ = nullptr;
    ID2D1SolidColorBrush* brushBorder_ = nullptr;
    ID2D1SolidColorBrush* brushInk_ = nullptr;
    ID2D1SolidColorBrush* brushMuted_ = nullptr;
    ID2D1SolidColorBrush* brushDim_ = nullptr;
    ID2D1SolidColorBrush* brushHair_ = nullptr;
    ID2D1SolidColorBrush* brushLabel_ = nullptr;
    ID2D1SolidColorBrush* brushSubtitle_ = nullptr;
    ID2D1SolidColorBrush* brushSurface_ = nullptr;
    ID2D1SolidColorBrush* brushSurfaceBorder_ = nullptr;
    ID2D1SolidColorBrush* brushStipple_ = nullptr;
    ID2D1SolidColorBrush* brushStippleBtn_ = nullptr;
    ID2D1SolidColorBrush* brushAccentHover_ = nullptr;
    ID2D1SolidColorBrush* brushIconBg_ = nullptr;
    ID2D1SolidColorBrush* brushIconFg_ = nullptr;
    ID2D1SolidColorBrush* brushGrain_ = nullptr;
    ID2D1SolidColorBrush* brushLeaf_ = nullptr;
    ID2D1SolidColorBrush* brushGold_ = nullptr;
    ID2D1SolidColorBrush* brushOnGold_ = nullptr;
    ID2D1BitmapBrush* grainBrush_ = nullptr;
    // Pre-rendered static chrome (icon badge glyph, corner leaf): collapsed ~55
    // per-frame FillRectangle calls into one tile blit each. Drawn with linear
    // interpolation so scaled-DPI rendering matches the old AA'd rect fills.
    ID2D1Bitmap* badgeGlyphTile_ = nullptr;
    ID2D1Bitmap* leafTile_ = nullptr;

    float sectionSpeedTestW_ = 0.0f;
    float sectionDnsW_ = 0.0f;
    bool haveLabelMetrics_ = false;
    // Label width cache: {text, fmt, maxW, measured width}. Text is copied so
    // mutable buffers (e.g. the link-speed label) remeasure when content changes.
    struct LabelWidthEntry {
        wchar_t text[32];
        IDWriteTextFormat* fmt;
        float maxW;
        float width;
    };
    static constexpr int kLabelWidthCacheSize = 16;
    LabelWidthEntry labelWidths_[kLabelWidthCacheSize]{};
    int labelWidthCount_ = 0;

    NetworkSnapshot snap_{};
    DnsProvider dnsProvider_ = DnsProvider::Dhcp;
    bool speedRunning_ = false;
    bool autostartEnabled_ = false;
    bool updateAvailable_ = false;
    bool updateBusy_ = false;
    wchar_t updateVersion_[32]{};
    wchar_t updateStatus_[96]{};

    VisualState visual_{};
    VisualState painted_{};

    D2D1_RECT_F closeBtn_{};
    D2D1_RECT_F iconBtn_{};
    D2D1_RECT_F runBtn_{};
    D2D1_RECT_F dnsBtns_[4]{};
    D2D1_RECT_F startupBtn_{};
    D2D1_RECT_F checkUpdateBtn_{};
    D2D1_RECT_F downloadUpdateBtn_{};

    DnsClickHandler onDnsClick_ = nullptr;
    SpeedClickHandler onSpeedClick_ = nullptr;
    VisibilityHandler onVisibility_ = nullptr;
    StartupToggleHandler onStartupToggle_ = nullptr;
    SimpleHandler onCheckUpdate_ = nullptr;
    SimpleHandler onDownloadUpdate_ = nullptr;

    PopupHit hoverHit_ = PopupHit::None;
    // TrackMouseEvent only needs arming once per enter/leave cycle.
    bool trackingMouse_ = false;
};
