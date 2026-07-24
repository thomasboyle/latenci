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

enum class PopupHit {
    None = 0,
    Close,
    RunSpeedTest,
    DnsDhcp,
    DnsCloudflare,
    DnsGoogle,
    DnsCustom,
};

class PopupWindow {
public:
    using DnsClickHandler = void (*)(DnsProvider);
    using SpeedClickHandler = void (*)();
    using VisibilityHandler = void (*)(bool visible);

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

    void ShowNearTray(const POINT& anchorScreen, ShowMode mode);
    void SetPinned();
    void Hide();

    // Used by main to decide hover dismiss: true if cursor is over popup client.
    bool ContainsScreenPoint(POINT screenPt) const;

    void SetSnapshot(const NetworkSnapshot& snap);
    void SetDnsProvider(DnsProvider provider);
    void SetSpeedTestRunning(bool running);

    void SetDnsClickHandler(DnsClickHandler handler) { onDnsClick_ = handler; }
    void SetSpeedClickHandler(SpeedClickHandler handler) { onSpeedClick_ = handler; }
    void SetVisibilityHandler(VisibilityHandler handler) { onVisibility_ = handler; }

private:
    // Everything DrawPanel reads, in already-formatted form. Compared byte-wise
    // against the last painted copy so unchanged 1 Hz ticks skip the repaint.
    // Zero-initialised on every rebuild so padding and post-terminator bytes
    // stay stable for memcmp.
    struct VisualState {
        wchar_t title[128];
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
        ShowMode mode;
        DnsProvider dns;
        PopupHit hover;
        bool speedRunning;
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
    void Layout();
    PopupHit HitTest(float x, float y) const;
    void ApplyPaperChrome();
    bool IsInteractiveHit(PopupHit hit) const;
    void BeginDrag();

    void RebuildVisual();
    void RefreshAndInvalidate();
    void ForceRepaint();
    float MeasureText(const wchar_t* text, IDWriteTextFormat* fmt, float maxW) const;
    void RefreshLabelMetrics();
    // Client pixels -> layout DIPs.
    float ToDips(int clientPixels) const;

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    ShowMode showMode_ = ShowMode::Hover;
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
    // Small wrapped tile instead of a full-panel grain surface.
    ID2D1BitmapBrush* grainBrush_ = nullptr;

    // Measured once per text-format lifetime; inputs are compile-time constants.
    float sectionSpeedTestW_ = 0.0f;
    float sectionDnsW_ = 0.0f;
    bool haveLabelMetrics_ = false;
    // Link-speed pill width, re-measured only when its label changes.
    wchar_t speedPillMeasuredFor_[32]{};
    float speedPillTextW_ = 0.0f;

    NetworkSnapshot snap_{};
    DnsProvider dnsProvider_ = DnsProvider::Dhcp;
    bool speedRunning_ = false;

    VisualState visual_{};
    VisualState painted_{};

    // Hit targets (client DIPs)
    D2D1_RECT_F closeBtn_{};
    D2D1_RECT_F runBtn_{};
    D2D1_RECT_F dnsBtns_[4]{};

    DnsClickHandler onDnsClick_ = nullptr;
    SpeedClickHandler onSpeedClick_ = nullptr;
    VisibilityHandler onVisibility_ = nullptr;

    PopupHit hoverHit_ = PopupHit::None;
};
