#include "TrayIcon.h"

namespace {

HICON CreateGlyphIcon(COLORREF bg, COLORREF fg) {
    const int size = 16;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colorBmp || !bits) {
        return nullptr;
    }

    auto* px = static_cast<DWORD*>(bits);
    const BYTE bgA = 255;
    const BYTE bgR = GetRValue(bg), bgG = GetGValue(bg), bgB = GetBValue(bg);
    const BYTE fgR = GetRValue(fg), fgG = GetGValue(fg), fgB = GetBValue(fg);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Rounded square badge
            const bool inBadge = (x >= 1 && x <= 14 && y >= 1 && y <= 14);
            const bool cornerCut =
                (x == 1 && y == 1) || (x == 14 && y == 1) ||
                (x == 1 && y == 14) || (x == 14 && y == 14);
            if (!inBadge || cornerCut) {
                px[y * size + x] = 0x00000000;
                continue;
            }
            px[y * size + x] = (bgA << 24) | (bgR << 16) | (bgG << 8) | bgB;
        }
    }

    // Simple ethernet port glyph (rectangle + pins)
    auto setFg = [&](int x, int y) {
        if (x >= 0 && x < size && y >= 0 && y < size) {
            px[y * size + x] = (255u << 24) | (fgR << 16) | (fgG << 8) | fgB;
        }
    };
    for (int x = 4; x <= 11; ++x) {
        setFg(x, 5);
        setFg(x, 10);
    }
    for (int y = 5; y <= 10; ++y) {
        setFg(4, y);
        setFg(11, y);
    }
    for (int x = 5; x <= 10; ++x) {
        setFg(x, 12);
    }
    setFg(6, 11);
    setFg(7, 11);
    setFg(8, 11);
    setFg(9, 11);

    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
}

}  // namespace

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(HINSTANCE /*instance*/, HWND messageHwnd, UINT callbackMsg) {
    iconConnected_ = CreateStateIcon(true);
    iconDisconnected_ = CreateStateIcon(false);

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = messageHwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = callbackMsg;
    nid_.hIcon = iconConnected_;
    wcscpy_s(nid_.szTip, L"Routing Crumbs");

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        return false;
    }

    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    created_ = true;
    return true;
}

void TrayIcon::Destroy() {
    if (created_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        created_ = false;
    }
    if (iconConnected_) {
        DestroyIcon(iconConnected_);
        iconConnected_ = nullptr;
    }
    if (iconDisconnected_) {
        DestroyIcon(iconDisconnected_);
        iconDisconnected_ = nullptr;
    }
}

void TrayIcon::SetConnected(bool connected) {
    if (connected_ == connected) {
        return;
    }
    connected_ = connected;
    UpdateIcon();
}

HICON TrayIcon::CreateStateIcon(bool connected) {
    if (connected) {
        // Cream badge + olive ink (matcha connected)
        return CreateGlyphIcon(RGB(0xF7, 0xF0, 0xE2), RGB(0x4F, 0x58, 0x38));
    }
    // Soft sage wash + muted olive (disconnected)
    return CreateGlyphIcon(RGB(0xC2, 0xC3, 0xA2), RGB(0x6B, 0x74, 0x4F));
}

void TrayIcon::UpdateIcon() {
    if (!created_) {
        return;
    }
    nid_.hIcon = connected_ ? iconConnected_ : iconDisconnected_;
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(nid_.szTip, connected_ ? L"Routing Crumbs — Connected" : L"Routing Crumbs — No connection");
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}