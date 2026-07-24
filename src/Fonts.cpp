#include "Fonts.h"
#include "resource.h"

#include <dwrite_3.h>

#pragma comment(lib, "dwrite.lib")

namespace {

IDWriteFactory5* g_factory5 = nullptr;
IDWriteInMemoryFontFileLoader* g_loader = nullptr;
IDWriteFontCollection1* g_collection = nullptr;
bool g_loaderRegistered = false;

const void* g_fontData = nullptr;
UINT32 g_fontSize = 0;

bool MapResourceBytes(HINSTANCE instance, int resId, const void** data, UINT32* size) {
    HRSRC hrsrc = FindResourceW(instance, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hrsrc) {
        return false;
    }
    HGLOBAL hglob = LoadResource(instance, hrsrc);
    if (!hglob) {
        return false;
    }
    void* locked = LockResource(hglob);
    const DWORD bytes = SizeofResource(instance, hrsrc);
    if (!locked || bytes == 0) {
        return false;
    }
    *data = locked;
    *size = bytes;
    return true;
}

}  // namespace

bool LoadAppFonts(HINSTANCE instance) {
    if (g_collection) {
        return true;
    }

    if (!MapResourceBytes(instance, IDR_FONT_PIXELIFY_REGULAR, &g_fontData, &g_fontSize)) {
        return false;
    }

    IDWriteFactory* baseFactory = nullptr;
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&baseFactory));
    if (FAILED(hr) || !baseFactory) {
        return false;
    }

    hr = baseFactory->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&g_factory5));
    baseFactory->Release();
    if (FAILED(hr) || !g_factory5) {
        return false;
    }

    hr = g_factory5->CreateInMemoryFontFileLoader(&g_loader);
    if (FAILED(hr) || !g_loader) {
        UnloadAppFonts();
        return false;
    }

    hr = g_factory5->RegisterFontFileLoader(g_loader);
    if (FAILED(hr)) {
        UnloadAppFonts();
        return false;
    }
    g_loaderRegistered = true;

    IDWriteFontFile* fontFile = nullptr;
    hr = g_loader->CreateInMemoryFontFileReference(
        g_factory5,
        g_fontData,
        g_fontSize,
        nullptr,
        &fontFile);
    if (FAILED(hr) || !fontFile) {
        UnloadAppFonts();
        return false;
    }

    IDWriteFontSetBuilder1* builder = nullptr;
    hr = g_factory5->CreateFontSetBuilder(&builder);
    if (FAILED(hr) || !builder) {
        fontFile->Release();
        UnloadAppFonts();
        return false;
    }

    hr = builder->AddFontFile(fontFile);
    fontFile->Release();
    if (FAILED(hr)) {
        builder->Release();
        UnloadAppFonts();
        return false;
    }

    IDWriteFontSet* fontSet = nullptr;
    hr = builder->CreateFontSet(&fontSet);
    builder->Release();
    if (FAILED(hr) || !fontSet) {
        UnloadAppFonts();
        return false;
    }

    hr = g_factory5->CreateFontCollectionFromFontSet(fontSet, &g_collection);
    fontSet->Release();
    if (FAILED(hr) || !g_collection) {
        UnloadAppFonts();
        return false;
    }
    return true;
}

void UnloadAppFonts() {
    if (g_collection) {
        g_collection->Release();
        g_collection = nullptr;
    }
    if (g_loader && g_factory5 && g_loaderRegistered) {
        g_factory5->UnregisterFontFileLoader(g_loader);
        g_loaderRegistered = false;
    }
    if (g_loader) {
        g_loader->Release();
        g_loader = nullptr;
    }
    if (g_factory5) {
        g_factory5->Release();
        g_factory5 = nullptr;
    }
    g_fontData = nullptr;
    g_fontSize = 0;
}

IDWriteFontCollection* GetAppFontCollection() {
    return g_collection;
}
