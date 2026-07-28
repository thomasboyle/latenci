#include "UpdateChecker.h"
#include "Version.h"

#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

#include <shellapi.h>

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/thomasboyle/latenci/releases/latest";
constexpr wchar_t kUserAgent[] = L"Latenci-Updater";

std::atomic<bool> g_stop{false};
HANDLE g_checkThread = nullptr;
HANDLE g_installThread = nullptr;

wchar_t g_remoteVersion[32]{};
wchar_t g_downloadUrl[512]{};
wchar_t g_status[96]{};
UpdateChecker::State g_state = UpdateChecker::State::Idle;
CRITICAL_SECTION g_cs;
bool g_csInit = false;
HWND g_notifyHwnd = nullptr;

void EnsureCs() {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
}

void SetState(UpdateChecker::State state, const wchar_t* status, HWND notify) {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    g_state = state;
    if (status) {
        wcsncpy_s(g_status, status, _TRUNCATE);
    } else {
        g_status[0] = L'\0';
    }
    LeaveCriticalSection(&g_cs);
    if (notify && IsWindow(notify)) {
        PostMessageW(notify, WM_APP_UPDATE_STATE, static_cast<WPARAM>(state), 0);
    }
}

bool ParseSemver(const char* s, int& major, int& minor, int& patch) {
    if (!s || !*s) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        ++s;
    }
    int a = 0, b = 0, c = 0;
    if (sscanf_s(s, "%d.%d.%d", &a, &b, &c) < 2) {
        return false;
    }
    major = a;
    minor = b;
    patch = c;
    return true;
}

int CompareSemver(const char* remote, const char* local) {
    int rMaj = 0, rMin = 0, rPat = 0;
    int lMaj = 0, lMin = 0, lPat = 0;
    if (!ParseSemver(remote, rMaj, rMin, rPat) || !ParseSemver(local, lMaj, lMin, lPat)) {
        return 0;
    }
    if (rMaj != lMaj) {
        return rMaj > lMaj ? 1 : -1;
    }
    if (rMin != lMin) {
        return rMin > lMin ? 1 : -1;
    }
    if (rPat != lPat) {
        return rPat > lPat ? 1 : -1;
    }
    return 0;
}

bool ExtractJsonString(const char* json, const char* key, char* out, size_t outN) {
    if (!json || !key || !out || outN == 0) {
        return false;
    }
    char pattern[96];
    sprintf_s(pattern, "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '"') {
        return false;
    }
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outN) {
        if (*p == '\\' && p[1]) {
            ++p;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

bool ExtractInstallerUrl(const char* json, char* out, size_t outN) {
    if (!json || !out || outN == 0) {
        return false;
    }
    const char* cursor = json;
    char best[512]{};
    char anyExe[512]{};
    while ((cursor = strstr(cursor, "\"browser_download_url\"")) != nullptr) {
        char url[512]{};
        if (!ExtractJsonString(cursor, "browser_download_url", url, sizeof(url))) {
            ++cursor;
            continue;
        }
        const char* name = strrchr(url, '/');
        name = name ? name + 1 : url;
        const bool isExe = strstr(name, ".exe") != nullptr;
        const bool isSetup = strstr(name, "Setup") != nullptr || strstr(name, "setup") != nullptr;
        const bool isNaked =
            _stricmp(name, "Latenci.exe") == 0 ||
            _stricmp(name, "RoutingCrumbs.exe") == 0;
        if (isExe && isSetup) {
            strcpy_s(best, url);
            break;
        }
        if (isExe && !isNaked && anyExe[0] == '\0') {
            strcpy_s(anyExe, url);
        }
        ++cursor;
    }
    if (best[0]) {
        strcpy_s(out, outN, best);
        return true;
    }
    if (anyExe[0]) {
        strcpy_s(out, outN, anyExe);
        return true;
    }
    return false;
}

bool Utf8ToWide(const char* utf8, wchar_t* out, size_t outChars) {
    if (!utf8 || !out || outChars == 0) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(outChars)) > 0;
}

bool HttpGetHttps(const wchar_t* host, const wchar_t* path, char** outBody, DWORD* outLen) {
    *outBody = nullptr;
    *outLen = 0;

    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    WinHttpAddRequestHeaders(request,
                             L"Accept: application/vnd.github+json\r\n",
                             static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    char* buf = nullptr;
    DWORD cap = 0;
    DWORD len = 0;
    for (;;) {
        if (g_stop.load()) {
            free(buf);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            break;
        }
        if (avail == 0) {
            break;
        }
        if (len + avail + 1 > cap) {
            const DWORD next = (cap == 0) ? (avail + 1 + 4096) : (cap * 2 + avail);
            char* grown = static_cast<char*>(realloc(buf, next));
            if (!grown) {
                free(buf);
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return false;
            }
            buf = grown;
            cap = next;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, buf + len, avail, &read) || read == 0) {
            break;
        }
        len += read;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!buf || len == 0) {
        free(buf);
        return false;
    }
    buf[len] = '\0';
    *outBody = buf;
    *outLen = len;
    return true;
}

bool CrackUrl(const wchar_t* url, wchar_t* host, size_t hostN, wchar_t* path, size_t pathN) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        return false;
    }
    if (!uc.lpszHostName || uc.dwHostNameLength == 0 || !uc.lpszUrlPath) {
        return false;
    }
    wcsncpy_s(host, hostN, uc.lpszHostName, uc.dwHostNameLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        swprintf_s(path, pathN, L"%.*s%.*s",
                   static_cast<int>(uc.dwUrlPathLength), uc.lpszUrlPath,
                   static_cast<int>(uc.dwExtraInfoLength), uc.lpszExtraInfo);
    } else {
        wcsncpy_s(path, pathN, uc.lpszUrlPath, uc.dwUrlPathLength);
    }
    return true;
}

bool DownloadFile(const wchar_t* url, const wchar_t* destPath) {
    wchar_t host[256]{};
    wchar_t path[1024]{};
    if (!CrackUrl(url, host, 256, path, 1024)) {
        return false;
    }

    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    HANDLE file = CreateFileW(destPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BYTE scratch[64 * 1024];
    bool success = true;
    for (;;) {
        if (g_stop.load()) {
            success = false;
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            success = false;
            break;
        }
        if (avail == 0) {
            break;
        }
        if (avail > sizeof(scratch)) {
            avail = sizeof(scratch);
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, scratch, avail, &read) || read == 0) {
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, scratch, read, &written, nullptr) || written != read) {
            success = false;
            break;
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!success) {
        DeleteFileW(destPath);
    }
    return success;
}

DWORD WINAPI CheckThread(LPVOID param) {
    const HWND notify = static_cast<HWND>(param);
    SetState(UpdateChecker::State::Checking, L"Checking for updates…", notify);

    char* body = nullptr;
    DWORD len = 0;
    if (!HttpGetHttps(kApiHost, kApiPath, &body, &len) || !body) {
        SetState(UpdateChecker::State::Failed, L"Update check failed", notify);
        return 0;
    }

    char tag[64]{};
    char url[512]{};
    const bool gotTag = ExtractJsonString(body, "tag_name", tag, sizeof(tag));
    const bool gotUrl = ExtractInstallerUrl(body, url, sizeof(url));
    free(body);

    if (!gotTag || !gotUrl) {
        SetState(UpdateChecker::State::Failed, L"Update check failed", notify);
        return 0;
    }

    if (CompareSemver(tag, APP_VERSION) <= 0) {
        EnsureCs();
        EnterCriticalSection(&g_cs);
        g_remoteVersion[0] = L'\0';
        g_downloadUrl[0] = L'\0';
        LeaveCriticalSection(&g_cs);
        SetState(UpdateChecker::State::UpToDate, L"You're up to date", notify);
        return 0;
    }

    EnsureCs();
    EnterCriticalSection(&g_cs);
    Utf8ToWide(tag, g_remoteVersion, 32);
    if (g_remoteVersion[0] == L'v' || g_remoteVersion[0] == L'V') {
        wmemmove(g_remoteVersion, g_remoteVersion + 1, wcslen(g_remoteVersion));
    }
    Utf8ToWide(url, g_downloadUrl, 512);
    LeaveCriticalSection(&g_cs);

    wchar_t status[96];
    swprintf_s(status, L"Update v%s available", g_remoteVersion);
    SetState(UpdateChecker::State::Available, status, notify);
    return 0;
}

DWORD WINAPI InstallThread(LPVOID param) {
    const HWND notify = static_cast<HWND>(param);
    SetState(UpdateChecker::State::Installing, L"Downloading update…", notify);

    wchar_t url[512]{};
    EnsureCs();
    EnterCriticalSection(&g_cs);
    wcsncpy_s(url, g_downloadUrl, _TRUNCATE);
    LeaveCriticalSection(&g_cs);

    if (!url[0]) {
        SetState(UpdateChecker::State::Failed, L"No download URL", notify);
        if (notify && IsWindow(notify)) {
            PostMessageW(notify, WM_APP_UPDATE_INSTALL_DONE, 0, 0);
        }
        return 0;
    }

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    wchar_t dest[MAX_PATH]{};
    swprintf_s(dest, L"%sLatenci-Update-Setup.exe", tempDir);

    const bool ok = DownloadFile(url, dest);
    if (ok) {
        const wchar_t* args =
            L"/VERYSILENT /CLOSEAPPLICATIONS /NORESTART /SUPPRESSMSGBOXES";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = dest;
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            SetState(UpdateChecker::State::Failed, L"Could not start installer", notify);
            if (notify && IsWindow(notify)) {
                PostMessageW(notify, WM_APP_UPDATE_INSTALL_DONE, 0, 0);
            }
            return 0;
        }
        if (sei.hProcess) {
            CloseHandle(sei.hProcess);
        }
        SetState(UpdateChecker::State::Installing, L"Installing…", notify);
    } else {
        SetState(UpdateChecker::State::Failed, L"Download failed", notify);
    }

    if (notify && IsWindow(notify)) {
        PostMessageW(notify, WM_APP_UPDATE_INSTALL_DONE, ok ? 1 : 0, 0);
    }
    return 0;
}

}  // namespace

namespace UpdateChecker {

void StartCheck(HWND notifyHwnd) {
    EnsureCs();
    g_notifyHwnd = notifyHwnd;
    g_stop.store(false);

    if (g_checkThread) {
        const DWORD wait = WaitForSingleObject(g_checkThread, 0);
        if (wait == WAIT_TIMEOUT) {
            return;
        }
        CloseHandle(g_checkThread);
        g_checkThread = nullptr;
    }

    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (g_state == State::Installing) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    LeaveCriticalSection(&g_cs);

    g_checkThread = CreateThread(nullptr, 0, CheckThread, notifyHwnd, 0, nullptr);
}

void Stop() {
    g_stop.store(true);
    if (g_checkThread) {
        WaitForSingleObject(g_checkThread, 5000);
        CloseHandle(g_checkThread);
        g_checkThread = nullptr;
    }
    if (g_installThread) {
        WaitForSingleObject(g_installThread, 15000);
        CloseHandle(g_installThread);
        g_installThread = nullptr;
    }
    if (g_csInit) {
        DeleteCriticalSection(&g_cs);
        g_csInit = false;
    }
}

bool HasUpdate() {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    const bool available = (g_state == State::Available || g_state == State::Installing)
        && g_remoteVersion[0] != L'\0';
    LeaveCriticalSection(&g_cs);
    return available;
}

State GetState() {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    const State s = g_state;
    LeaveCriticalSection(&g_cs);
    return s;
}

const wchar_t* AvailableVersion() {
    return g_remoteVersion;
}

const wchar_t* StatusText() {
    return g_status;
}

void BeginInstall(HWND notifyHwnd) {
    if (notifyHwnd) {
        g_notifyHwnd = notifyHwnd;
    }
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (g_state == State::Installing || g_downloadUrl[0] == L'\0') {
        LeaveCriticalSection(&g_cs);
        return;
    }
    LeaveCriticalSection(&g_cs);

    if (g_installThread) {
        const DWORD wait = WaitForSingleObject(g_installThread, 0);
        if (wait == WAIT_TIMEOUT) {
            return;
        }
        CloseHandle(g_installThread);
        g_installThread = nullptr;
    }
    g_installThread = CreateThread(nullptr, 0, InstallThread, g_notifyHwnd, 0, nullptr);
}

void OnInstallFinished(bool ok) {
    if (!ok) {
        SetState(State::Failed, L"Update failed", g_notifyHwnd);
    }
}

}  // namespace UpdateChecker
