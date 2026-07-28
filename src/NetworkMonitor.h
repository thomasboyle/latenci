#pragma once
#include "Messages.h"
#include <atomic>

class NetworkMonitor {
public:
    NetworkMonitor();
    ~NetworkMonitor();
    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;
    bool Start(HWND notifyHwnd, UINT notifyMsg);
    void Stop();
    // When true, poll thread posts every tick for live popup rates and the ping
    // thread samples at 1 Hz. When false, posts only on connected-bit changes
    // (tray icon) and the ping thread parks until the popup reopens.
    void SetLiveUpdates(bool enabled);
    NetworkSnapshot GetSnapshot() const;
    bool Connected() const;
    void SetPingTarget(const wchar_t* target);
    void SetSpeedResults(double downloadMbps, double uploadMbps, bool running);
    // Cached adapter identity — only mutated on network-change path.
    GUID CachedGuid() const;
    bool HasAdapter() const;
private:
    static DWORD WINAPI PollThreadEntry(LPVOID param);
    static DWORD WINAPI PingThreadEntry(LPVOID param);
    void PollThreadMain();
    void PingThreadMain();
    void ResolveStaticAdapterInfo();
    void PollDynamicStats();
    void ResolvePingAddress();
    bool PingOnce(double& outMs) const;
    void ResetPingWindow();
    void NotifyIfNeeded(bool connected);
    // Poll-thread only: (re)establish session byte baseline. Returns true when
    // totals/rates must be cleared (new adapter LUID or counter rewind).
    bool EnsureSessionBaseline(NET_LUID luid, ULONG64 inOctets, ULONG64 outOctets);
    void ResetSessionBaseline(NET_LUID luid, ULONG64 inOctets, ULONG64 outOctets);
    void JoinWorkerThreads();
    void ReleaseNativeHandles();
    static void CALLBACK OnIpInterfaceChange(
        PVOID callerContext,
        PMIB_IPINTERFACE_ROW row,
        MIB_NOTIFICATION_TYPE notificationType);
    HWND notifyHwnd_ = nullptr;
    UINT notifyMsg_ = 0;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    NetworkStaticState static_{};
    NetworkDynamicState dynamic_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> liveUpdates_{false};
    HANDLE pollThread_ = nullptr;
    HANDLE pingThread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    // Auto-reset: wakes the parked ping thread when the popup becomes visible.
    HANDLE pingWakeEvent_ = nullptr;
    HANDLE changeHandle_ = nullptr;
    std::atomic<bool> dirtyStatic_{true};
    wchar_t pingTarget_[64]{L"1.1.1.1"};
    IN_ADDR pingAddr_{};
    std::atomic<bool> pingAddrValid_{false};
    HANDLE icmpHandle_ = INVALID_HANDLE_VALUE;
    HANDLE wlanHandle_ = nullptr;
    // Reused GetAdaptersAddresses scratch buffer (poll thread only).
    unsigned char* gaaBuf_ = nullptr;
    ULONG gaaCapacity_ = 0;
    // Wi-Fi band cache (poll thread only). The underlying BSS query needs a
    // forced scan, so it is rate-limited rather than run on every re-resolve.
    static constexpr ULONGLONG kBandTtlMs = 15000;
    GUID bandGuid_{};
    wchar_t bandLabel_[32]{L"—"};
    ULONGLONG bandQueryTick_ = 0;
    bool haveBand_ = false;
    // Rolling ping window for packet-loss %
    static constexpr int kPingWindow = 20;
    bool pingOk_[kPingWindow]{};
    int pingCount_ = 0;
    int pingIndex_ = 0;
    int pingFails_ = 0;
    // Byte counters for rate calculation (poll thread only)
    ULONG64 lastInOctets_ = 0;
    ULONG64 lastOutOctets_ = 0;
    ULONG64 baselineIn_ = 0;
    ULONG64 baselineOut_ = 0;
    bool haveCounters_ = false;
    // Session totals persist for the process lifetime per adapter LUID.
    // Not cleared on IP-interface churn or brief adapter loss.
    NET_LUID sessionLuid_{};
    bool haveSessionBaseline_ = false;
    ULONGLONG lastCounterTick_ = 0;
    // Poll-thread only: last connected value we posted about.
    bool havePostedConnected_ = false;
    bool lastPostedConnected_ = false;
};
