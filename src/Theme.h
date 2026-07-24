#pragma once

namespace Theme {

// Panel — height fits content through DNS buttons + bottom padding (no spare footer).
constexpr float kPanelWidth        = 440.0f;
constexpr float kCornerRadius      = 10.0f;
constexpr float kPadding           = 14.0f;
constexpr float kRowGap            = 12.0f;
constexpr float kSectionGap        = 16.0f;
constexpr float kBorderWidth       = 1.0f;
constexpr float kLabelFieldGap     = 4.0f;

// Colors (sRGB 0–1) — 8-bit cottagecore / matcha
struct Color {
    float r, g, b, a;
};

constexpr Color FromRgb(unsigned r, unsigned g, unsigned b, float a = 1.0f) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}

constexpr Color kPageBg          = FromRgb(0xE8, 0xDF, 0xD0);
constexpr Color kPanel           = FromRgb(0xF7, 0xF0, 0xE2);
constexpr Color kPanelBorder     = FromRgb(0xA7, 0xA2, 0x8B);
constexpr Color kInk             = FromRgb(0x2A, 0x32, 0x20);
constexpr Color kInkMuted        = FromRgb(0x2A, 0x32, 0x20, 0.75f);
constexpr Color kLabel           = FromRgb(0x5A, 0x56, 0x4C);
constexpr Color kSubtitle        = FromRgb(0x4F, 0x58, 0x38);
constexpr Color kSurface         = FromRgb(0xFF, 0xFB, 0xF3);
constexpr Color kSurfaceBorder   = FromRgb(0x6B, 0x74, 0x4F);
constexpr Color kStipple         = FromRgb(0xC2, 0xC3, 0xA2);
constexpr Color kStippleBtn      = FromRgb(0xA8, 0xAA, 0x8C);
constexpr Color kAccentHover     = FromRgb(0x7E, 0x89, 0x5F);
constexpr Color kTextDim         = FromRgb(0x2A, 0x32, 0x20, 0.55f);
constexpr Color kIconBadgeBg     = FromRgb(0xE4, 0xE8, 0xD4);
constexpr Color kIconBadgeFg     = FromRgb(0x4F, 0x58, 0x38);
constexpr Color kGrain           = FromRgb(70, 60, 45, 0.30f);
constexpr Color kHairline        = FromRgb(0xA7, 0xA2, 0x8B, 0.55f);
constexpr Color kLeaf            = FromRgb(0x5F, 0x6B, 0x45);

// Typography — Pixelify Sans (Regular; Bold weight falls back to Regular)
constexpr wchar_t kFontFamily[]  = L"Pixelify Sans";
constexpr wchar_t kFontFallback[] = L"Courier New";
constexpr float kTitleSize       = 20.0f;
constexpr float kBrandSize       = 12.0f;
constexpr float kLabelSize       = 14.0f;
constexpr float kValueSize       = 14.0f;
constexpr float kSectionSize     = 16.0f;
constexpr float kPillSize        = 14.0f;

// Layout pieces
constexpr float kIconBadgeSize   = 28.0f;
constexpr float kIconBadgeRadius = 4.0f;
constexpr float kSpeedPillH      = 26.0f;
constexpr float kSpeedPillPadX   = 12.0f;
constexpr float kSegButtonH      = 30.0f;
constexpr float kRunButtonH      = 34.0f;
constexpr float kRunButtonW      = 64.0f;
constexpr float kControlRadius   = 4.0f;
constexpr float kBtnBorderBottom = 3.0f;
constexpr float kBtnBorderSide   = 1.0f;
constexpr float kSecondaryBottom = 2.0f;

// Derived from layout steps in PopupWindow::DrawPanel (ends after DNS pills).
constexpr float kPanelHeight =
    kPadding
    + kIconBadgeSize
    + kSectionGap
    + 4.0f * (kLabelSize + kRowGap)
    + 2.0f + kSectionGap
    + kRunButtonH + kRowGap
    + kLabelSize + kRowGap
    + 2.0f + kSectionGap
    + kSectionSize + kLabelFieldGap + kRowGap
    + kSegButtonH
    + kPadding;

// Flyout behavior
constexpr UINT kHoverOpenDelayMs = 280;

// Close button (pinned mode)
constexpr float kCloseSize       = 14.0f;
constexpr float kCloseHitPad     = 8.0f;
constexpr float kCloseStroke     = 1.5f;
constexpr float kHeaderDragH     = 52.0f;

}  // namespace Theme
