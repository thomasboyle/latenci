#pragma once

#include <windows.h>

// Posted when update state changes (wParam = UpdateChecker::State).
inline constexpr UINT WM_APP_UPDATE_STATE = WM_APP + 20;
// Posted after download+launch of the installer (wParam nonzero = success).
inline constexpr UINT WM_APP_UPDATE_INSTALL_DONE = WM_APP + 21;

namespace UpdateChecker {

enum class State : WPARAM {
    Idle = 0,
    Checking,
    UpToDate,
    Available,
    Installing,
    Failed,
};

// Background check on launch (and reusable for manual "Check for updates").
void StartCheck(HWND notifyHwnd);
void Stop();

bool HasUpdate();
State GetState();
const wchar_t* AvailableVersion();
const wchar_t* StatusText();

// Download the installer and run a silent update (background thread).
void BeginInstall(HWND notifyHwnd);

// Call on the UI thread after WM_APP_UPDATE_INSTALL_DONE.
void OnInstallFinished(bool ok);

}  // namespace UpdateChecker
