#include "SpeedTest.h"

#include <winhttp.h>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr wchar_t kDownloadHost[] = L"speed.cloudflare.com";
constexpr wchar_t kUploadPath[]   = L"/__up";

constexpr size_t kFullDownloadBytes = 25 * 1024 * 1024;
constexpr size_t kQuickDownloadBytes = 2 * 1024 * 1024;
constexpr size_t kFullUploadBytes = 10 * 1024 * 1024;
constexpr size_t kQuickUploadBytes = 1 * 1024 * 1024;

constexpr int kFullMaxMs = 8000;
constexpr int kQuickMaxMs = 2500;
constexpr int kMinMeasureMs = 500;

// Single reused transfer buffer: the upload streams this repeatedly instead of
// materialising the whole payload, and the download reads into it.
constexpr DWORD kScratchBytes = 64 * 1024;

LONGLONG NowTicks() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double SecondsSince(LONGLONG startTicks) {
    static LONGLONG freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    if (freq == 0) {
        return 0.0;
    }
    return static_cast<double>(NowTicks() - startTicks) / static_cast<double>(freq);
}

double ElapsedMbps(ULONGLONG bytes, double seconds) {
    if (seconds <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) * 8.0) / (seconds * 1'000'000.0);
}

bool RunDownload(HINTERNET connect, bool quick, const std::atomic<bool>& cancel,
                 BYTE* scratch, double& outMbps, ErrorMsg& error) {
    const size_t wantBytes = quick ? kQuickDownloadBytes : kFullDownloadBytes;
    const int maxMs = quick ? kQuickMaxMs : kFullMaxMs;

    wchar_t path[64];
    swprintf_s(path, L"/__down?bytes=%zu", wantBytes);

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        error.Set(L"OpenRequest failed");
        return false;
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        error.Set(L"Download request failed");
        WinHttpCloseHandle(request);
        return false;
    }

    ULONGLONG total = 0;
    const LONGLONG t0 = NowTicks();

    for (;;) {
        if (cancel.load()) {
            error.Set(L"Cancelled");
            WinHttpCloseHandle(request);
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            break;
        }
        if (avail == 0) {
            break;
        }
        if (avail > kScratchBytes) {
            avail = kScratchBytes;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request, scratch, avail, &read) || read == 0) {
            break;
        }
        total += read;

        const double ms = SecondsSince(t0) * 1000.0;
        if (ms >= maxMs && total > 256 * 1024) {
            break;
        }
    }

    const double seconds = SecondsSince(t0);
    outMbps = ElapsedMbps(total, seconds);

    WinHttpCloseHandle(request);

    if (total == 0) {
        error.Set(L"No download data");
        return false;
    }
    return true;
}

bool RunUpload(HINTERNET connect, bool quick, const std::atomic<bool>& cancel,
               BYTE* scratch, double& outMbps, ErrorMsg& error) {
    const size_t uploadBytes = quick ? kQuickUploadBytes : kFullUploadBytes;

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"POST",
        kUploadPath,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        error.Set(L"OpenRequest failed");
        return false;
    }

    memset(scratch, 0x5A, kScratchBytes);
    const LONGLONG t0 = NowTicks();

    // Stream the body in chunks: bounds memory to one buffer and gives the
    // cancel flag a check between writes so shutdown never waits out an upload.
    BOOL ok = WinHttpSendRequest(
        request,
        L"Content-Type: application/octet-stream\r\n",
        (DWORD)-1,
        WINHTTP_NO_REQUEST_DATA,
        0,
        static_cast<DWORD>(uploadBytes),
        0);

    size_t sent = 0;
    bool cancelled = false;
    while (ok && sent < uploadBytes) {
        if (cancel.load()) {
            cancelled = true;
            break;
        }
        const size_t remaining = uploadBytes - sent;
        const DWORD toWrite = remaining < kScratchBytes
            ? static_cast<DWORD>(remaining)
            : kScratchBytes;
        DWORD written = 0;
        if (!WinHttpWriteData(request, scratch, toWrite, &written) || written == 0) {
            ok = FALSE;
            break;
        }
        sent += written;
    }

    if (cancelled) {
        error.Set(L"Cancelled");
        WinHttpCloseHandle(request);
        return false;
    }
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        error.Set(L"Upload request failed");
        WinHttpCloseHandle(request);
        return false;
    }

    DWORD read = 0;
    while (WinHttpReadData(request, scratch, kScratchBytes, &read) && read > 0) {
        if (cancel.load()) {
            break;
        }
    }

    const double seconds = SecondsSince(t0);
    const double floorSeconds = kMinMeasureMs / 1000.0;
    outMbps = ElapsedMbps(sent, seconds > floorSeconds ? seconds : floorSeconds);

    WinHttpCloseHandle(request);
    return !cancel.load();
}

}  // namespace

SpeedTest::SpeedTest() = default;

SpeedTest::~SpeedTest() {
    Cancel();
}

DWORD WINAPI SpeedTest::WorkerEntry(LPVOID param) {
    static_cast<SpeedTest*>(param)->Worker();
    return 0;
}

void SpeedTest::Start(HWND notifyHwnd, UINT doneMsg, DoneCallback onDone, bool quick) {
    if (running_.exchange(true)) {
        return;
    }
    cancel_ = false;
    notifyHwnd_ = notifyHwnd;
    doneMsg_ = doneMsg;
    onDone_ = onDone;
    quick_ = quick;
    if (worker_) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
    }
    worker_ = CreateThread(nullptr, 0, WorkerEntry, this, 0, nullptr);
    if (!worker_) {
        running_ = false;
    }
}

void SpeedTest::CloseSession() {
    void* handle = nullptr;
    AcquireSRWLockExclusive(&sessionLock_);
    handle = session_;
    session_ = nullptr;
    ReleaseSRWLockExclusive(&sessionLock_);
    if (handle) {
        // Aborts any transfer in flight on this session.
        WinHttpCloseHandle(handle);
    }
}

void SpeedTest::Cancel() {
    cancel_ = true;
    CloseSession();
    if (worker_) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
    }
    running_ = false;
}

void SpeedTest::Worker() {
    Result result;

    BYTE* scratch = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, kScratchBytes));
    if (!scratch) {
        result.error.Set(L"Out of memory");
        if (onDone_) {
            onDone_(result);
        }
        if (notifyHwnd_ && IsWindow(notifyHwnd_)) {
            PostMessageW(notifyHwnd_, doneMsg_, 0, 0);
        }
        running_ = false;
        return;
    }

    HINTERNET session = WinHttpOpen(
        L"RoutingCrumbs/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        result.ok = false;
        result.error.Set(L"WinHttpOpen failed");
    } else {
        AcquireSRWLockExclusive(&sessionLock_);
        const bool aborted = cancel_.load();
        if (!aborted) {
            session_ = session;
        }
        ReleaseSRWLockExclusive(&sessionLock_);
        if (aborted) {
            WinHttpCloseHandle(session);
            session = nullptr;
            result.ok = false;
            result.error.Set(L"Cancelled");
        }
    }

    if (session) {
        HINTERNET connect = WinHttpConnect(session, kDownloadHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) {
            result.ok = false;
            result.error.Set(L"Connect failed");
        } else {
            if (!RunDownload(connect, quick_, cancel_, scratch, result.downloadMbps, result.error)) {
                result.ok = false;
            } else if (!RunUpload(connect, quick_, cancel_, scratch, result.uploadMbps, result.error)) {
                result.ok = false;
            } else {
                result.ok = true;
            }
            WinHttpCloseHandle(connect);
        }
        CloseSession();
    }

    HeapFree(GetProcessHeap(), 0, scratch);

    if (onDone_) {
        onDone_(result);
    }
    if (notifyHwnd_ && IsWindow(notifyHwnd_)) {
        PostMessageW(notifyHwnd_, doneMsg_, result.ok ? 1 : 0, 0);
    }
    running_ = false;
}
