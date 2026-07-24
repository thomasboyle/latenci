#pragma once

#include "Messages.h"

#include <atomic>

class SpeedTest {
public:
    struct Result {
        bool ok = false;
        double downloadMbps = 0.0;
        double uploadMbps = 0.0;
        ErrorMsg error{};
    };

    using DoneCallback = void (*)(const Result&);

    SpeedTest();
    ~SpeedTest();

    SpeedTest(const SpeedTest&) = delete;
    SpeedTest& operator=(const SpeedTest&) = delete;

    bool IsRunning() const { return running_.load(); }

    // quick: smaller payloads / shorter window for startup population.
    void Start(HWND notifyHwnd, UINT doneMsg, DoneCallback onDone = nullptr, bool quick = false);
    void Cancel();

private:
    static DWORD WINAPI WorkerEntry(LPVOID param);
    void Worker();
    void CloseSession();

    HWND notifyHwnd_ = nullptr;
    UINT doneMsg_ = 0;
    DoneCallback onDone_ = nullptr;
    bool quick_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    HANDLE worker_ = nullptr;
    // Guards session_ so Cancel() and Worker() cannot both close it.
    SRWLOCK sessionLock_ = SRWLOCK_INIT;
    void* session_ = nullptr;
};
