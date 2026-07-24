#pragma once

#include <dwrite.h>
#include <windows.h>

// Loads Pixelify Sans (Regular) into a DirectWrite private font collection.
bool LoadAppFonts(HINSTANCE instance);
void UnloadAppFonts();

// Valid after LoadAppFonts; may be null if load failed (caller should fall back).
IDWriteFontCollection* GetAppFontCollection();
