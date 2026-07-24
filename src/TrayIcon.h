#pragma once

#include "WinIncludes.h"

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Create(HINSTANCE instance, HWND messageHwnd, UINT callbackMsg);
    void Destroy();

    void SetConnected(bool connected);

private:
    HICON CreateStateIcon(bool connected);
    void UpdateIcon();

    NOTIFYICONDATAW nid_{};
    bool created_ = false;
    bool connected_ = true;
    HICON iconConnected_ = nullptr;
    HICON iconDisconnected_ = nullptr;
};
