#include "NetworkMonitor.h"
#include <combaseapi.h>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wlanapi.lib")

namespace {

// Exclusive SRWLOCK guard; replaces std::mutex/std::lock_guard so the binary
// does not import the C++ runtime's threading shims.
struct Guard {
    PSRWLOCK lock;
    explicit Guard(PSRWLOCK l) : lock(l) { AcquireSRWLockExclusive(lock); }
    ~Guard() { ReleaseSRWLockExclusive(lock); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// First GetAdaptersAddresses call allocates this much; grown on overflow only.
constexpr ULONG kGaaInitialBytes = 16 * 1024;

void FormatLinkSpeed(ULONG64 bps, wchar_t* out, size_t outCount) {
    if (bps >= 1'000'000'000ull) {
        const double g = static_cast<double>(bps) / 1'000'000'000.0;
        swprintf_s(out, outCount, L"%.0fgbit", g);
    } else if (bps >= 1'000'000ull) {
        const double m = static_cast<double>(bps) / 1'000'000.0;
        swprintf_s(out, outCount, L"%.0fmbit", m);
    } else {
        swprintf_s(out, outCount, L"%llubit", static_cast<unsigned long long>(bps));
    }
}

void FormatWifiBand(ULONG freqKhz, wchar_t* out, size_t outCount) {
    if (freqKhz == 0) {
        wcscpy_s(out, outCount, L"—");
        return;
    }
    // ulChCenterFrequency is in kHz
    if (freqKhz < 3'000'000ul) {
        wcscpy_s(out, outCount, L"2.4 GHz");
    } else if (freqKhz < 6'000'000ul) {
        wcscpy_s(out, outCount, L"5 GHz");
    } else {
        wcscpy_s(out, outCount, L"6 GHz");
    }
}

void QueryWifiFrequency(HANDLE wlanClient, const GUID& ifaceGuid, wchar_t* out, size_t outCount) {
    wcscpy_s(out, outCount, L"—");
    if (!wlanClient) {
        return;
    }
    DWORD dataSize = 0;
    PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
    WLAN_OPCODE_VALUE_TYPE opType{};
    const DWORD qerr = WlanQueryInterface(
        wlanClient,
        &ifaceGuid,
        wlan_intf_opcode_current_connection,
        nullptr,
        &dataSize,
        reinterpret_cast<PVOID*>(&attrs),
        &opType);
    if (qerr != ERROR_SUCCESS || !attrs) {
        return;
    }
    if (attrs->isState != wlan_interface_state_connected) {
        WlanFreeMemory(attrs);
        return;
    }
    PWLAN_BSS_LIST bssList = nullptr;
    // bForceScan must stay TRUE: with FALSE the cached list comes back empty and
    // the band is never resolved. The caller rate-limits this instead.
    const DWORD berr = WlanGetNetworkBssList(
        wlanClient,
        &ifaceGuid,
        &attrs->wlanAssociationAttributes.dot11Ssid,
        attrs->wlanAssociationAttributes.dot11BssType,
        TRUE,
        nullptr,
        &bssList);
    ULONG freqKhz = 0;
    if (berr == ERROR_SUCCESS && bssList) {
        const auto& bssid = attrs->wlanAssociationAttributes.dot11Bssid;
        for (DWORD i = 0; i < bssList->dwNumberOfItems; ++i) {
            const auto& entry = bssList->wlanBssEntries[i];
            if (memcmp(entry.dot11Bssid, bssid, sizeof(DOT11_MAC_ADDRESS)) == 0) {
                freqKhz = entry.ulChCenterFrequency;
                break;
            }
        }
        if (freqKhz == 0 && bssList->dwNumberOfItems > 0) {
            freqKhz = bssList->wlanBssEntries[0].ulChCenterFrequency;
        }
        WlanFreeMemory(bssList);
    }
    WlanFreeMemory(attrs);
    FormatWifiBand(freqKhz, out, outCount);
}

bool IsEthernetLike(IFTYPE type) {
    return type == IF_TYPE_ETHERNET_CSMACD ||
           type == IF_TYPE_IEEE80211 ||
           type == IF_TYPE_PPP ||
           type == IF_TYPE_PROP_VIRTUAL;
}

// Interface-change bursts re-resolve identical data; only bump the generation
// when a label the popup actually renders moved.
bool StaticLabelsDiffer(const NetworkStaticState& a, const NetworkStaticState& b) {
    return a.adapterValid != b.adapterValid ||
           a.ifIndex != b.ifIndex ||
           a.luid.Value != b.luid.Value ||
           memcmp(&a.interfaceGuid, &b.interfaceGuid, sizeof(GUID)) != 0 ||
           wcscmp(a.adapterName, b.adapterName) != 0 ||
           wcscmp(a.ipAddress, b.ipAddress) != 0 ||
           wcscmp(a.frequency, b.frequency) != 0 ||
           wcscmp(a.linkSpeedLabel, b.linkSpeedLabel) != 0;
}

}  // namespace

NetworkMonitor::NetworkMonitor() {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pingWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

NetworkMonitor::~NetworkMonitor() {
    Stop();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (pingWakeEvent_) {
        CloseHandle(pingWakeEvent_);
        pingWakeEvent_ = nullptr;
    }
    if (gaaBuf_) {
        HeapFree(GetProcessHeap(), 0, gaaBuf_);
        gaaBuf_ = nullptr;
        gaaCapacity_ = 0;
    }
}

DWORD WINAPI NetworkMonitor::PollThreadEntry(LPVOID param) {
    static_cast<NetworkMonitor*>(param)->PollThreadMain();
    return 0;
}

DWORD WINAPI NetworkMonitor::PingThreadEntry(LPVOID param) {
    static_cast<NetworkMonitor*>(param)->PingThreadMain();
    return 0;
}

bool NetworkMonitor::Start(HWND notifyHwnd, UINT notifyMsg) {
    if (running_) {
        return true;
    }
    notifyHwnd_ = notifyHwnd;
    notifyMsg_ = notifyMsg;
    ResetEvent(stopEvent_);
    ResetEvent(pingWakeEvent_);
    dirtyStatic_ = true;
    pingAddrValid_ = false;
    havePostedConnected_ = false;
    liveUpdates_ = false;
    icmpHandle_ = IcmpCreateFile();
    if (icmpHandle_ == INVALID_HANDLE_VALUE) {
        // Continue without ICMP; rates / adapter info still work.
        icmpHandle_ = INVALID_HANDLE_VALUE;
    }
    DWORD wlanVer = 0;
    if (WlanOpenHandle(2, nullptr, &wlanVer, &wlanHandle_) != ERROR_SUCCESS) {
        wlanHandle_ = nullptr;
    }
    const DWORD err = NotifyIpInterfaceChange(
        AF_UNSPEC,
        OnIpInterfaceChange,
        this,
        TRUE,
        &changeHandle_);
    if (err != NO_ERROR && err != ERROR_IO_PENDING) {
        // Continue without change notifications; static resolve still runs once.
        changeHandle_ = nullptr;
    }
    running_ = true;
    pollThread_ = CreateThread(nullptr, 0, PollThreadEntry, this, 0, nullptr);
    pingThread_ = CreateThread(nullptr, 0, PingThreadEntry, this, 0, nullptr);
    return true;
}

void NetworkMonitor::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    SetEvent(stopEvent_);
    SetEvent(pingWakeEvent_);
    if (pollThread_) {
        WaitForSingleObject(pollThread_, INFINITE);
        CloseHandle(pollThread_);
        pollThread_ = nullptr;
    }
    if (pingThread_) {
        WaitForSingleObject(pingThread_, INFINITE);
        CloseHandle(pingThread_);
        pingThread_ = nullptr;
    }
    if (changeHandle_) {
        CancelMibChangeNotify2(changeHandle_);
        changeHandle_ = nullptr;
    }
    if (icmpHandle_ != INVALID_HANDLE_VALUE) {
        IcmpCloseHandle(icmpHandle_);
        icmpHandle_ = INVALID_HANDLE_VALUE;
    }
    if (wlanHandle_) {
        WlanCloseHandle(wlanHandle_, nullptr);
        wlanHandle_ = nullptr;
    }
}

void NetworkMonitor::SetLiveUpdates(bool enabled) {
    liveUpdates_.store(enabled, std::memory_order_release);
    if (enabled) {
        // Release the parked ping thread so the popup gets a fresh sample now.
        SetEvent(pingWakeEvent_);
        if (notifyHwnd_ && IsWindow(notifyHwnd_)) {
            // Immediate refresh when the popup opens; don't wait for the next poll tick.
            PostMessageW(notifyHwnd_, notifyMsg_, 0, 0);
        }
    }
}

NetworkSnapshot NetworkMonitor::GetSnapshot() const {
    Guard guard(&lock_);
    return MergeSnapshot(static_, dynamic_);
}

bool NetworkMonitor::Connected() const {
    Guard guard(&lock_);
    return dynamic_.connected;
}

void NetworkMonitor::SetPingTarget(const wchar_t* target) {
    Guard guard(&lock_);
    wcsncpy_s(pingTarget_, target, _TRUNCATE);
    pingAddrValid_ = false;
}

void NetworkMonitor::SetSpeedResults(double downloadMbps, double uploadMbps, bool running) {
    Guard guard(&lock_);
    dynamic_.downloadMbps = downloadMbps;
    dynamic_.uploadMbps = uploadMbps;
    dynamic_.speedTestRunning = running;
}

GUID NetworkMonitor::CachedGuid() const {
    Guard guard(&lock_);
    return static_.interfaceGuid;
}

bool NetworkMonitor::HasAdapter() const {
    Guard guard(&lock_);
    return static_.adapterValid;
}

void CALLBACK NetworkMonitor::OnIpInterfaceChange(
    PVOID callerContext,
    PMIB_IPINTERFACE_ROW /*row*/,
    MIB_NOTIFICATION_TYPE /*notificationType*/) {
    auto* self = static_cast<NetworkMonitor*>(callerContext);
    if (self) {
        self->dirtyStatic_ = true;
    }
}

void NetworkMonitor::NotifyIfNeeded(bool connected) {
    const bool connectedChanged =
        !havePostedConnected_ || connected != lastPostedConnected_;
    if (!connectedChanged && !liveUpdates_.load(std::memory_order_acquire)) {
        return;
    }
    havePostedConnected_ = true;
    lastPostedConnected_ = connected;
    if (notifyHwnd_ && IsWindow(notifyHwnd_)) {
        PostMessageW(notifyHwnd_, notifyMsg_, 0, 0);
    }
}

void NetworkMonitor::PollThreadMain() {
    while (running_) {
        const bool resolvedStatic = dirtyStatic_.exchange(false);
        if (resolvedStatic) {
            ResolveStaticAdapterInfo();
        } else {
            PollDynamicStats();
        }
        bool connected = false;
        {
            Guard guard(&lock_);
            connected = dynamic_.connected;
        }
        NotifyIfNeeded(connected);
        const DWORD wait = WaitForSingleObject(stopEvent_, 1000);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }
}

void NetworkMonitor::ResetPingWindow() {
    Guard guard(&lock_);
    pingCount_ = 0;
    pingIndex_ = 0;
    pingFails_ = 0;
    dynamic_.packetLossPct = 0.0;
}

void NetworkMonitor::PingThreadMain() {
    while (running_) {
        if (!liveUpdates_.load(std::memory_order_acquire)) {
            // Nothing renders latency while the popup is hidden. Park instead of
            // emitting an ICMP echo every second for a value nobody reads; the
            // loss window restarts so it always covers observed time only.
            ResetPingWindow();
            const HANDLE waits[2] = {stopEvent_, pingWakeEvent_};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 || !running_) {
                break;
            }
            continue;
        }
        if (!pingAddrValid_.load(std::memory_order_acquire)) {
            ResolvePingAddress();
        }
        double ms = -1.0;
        const bool ok = PingOnce(ms);
        {
            Guard guard(&lock_);
            ApplyPingResult(ok, ms);
        }
        const HANDLE waits[2] = {stopEvent_, pingWakeEvent_};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }
}

void NetworkMonitor::ResolvePingAddress() {
    wchar_t target[64];
    {
        Guard guard(&lock_);
        wcsncpy_s(target, pingTarget_, _TRUNCATE);
    }
    IN_ADDR addr{};
    bool ok = false;
    if (InetPtonW(AF_INET, target, &addr) == 1) {
        ok = true;
    } else {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        ADDRINFOW* res = nullptr;
        if (GetAddrInfoW(target, nullptr, &hints, &res) == 0 && res) {
            addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            FreeAddrInfoW(res);
            ok = true;
        }
    }
    {
        Guard guard(&lock_);
        pingAddr_ = addr;
        pingAddrValid_.store(ok, std::memory_order_release);
    }
}

void NetworkMonitor::ApplyPingResult(bool ok, double ms) {
    if (pingCount_ >= kPingWindow) {
        if (!pingOk_[pingIndex_]) {
            --pingFails_;
        }
    }
    pingOk_[pingIndex_] = ok;
    if (!ok) {
        ++pingFails_;
    }
    pingIndex_ = (pingIndex_ + 1) % kPingWindow;
    if (pingCount_ < kPingWindow) {
        ++pingCount_;
    }
    dynamic_.packetLossPct = pingCount_ > 0
        ? (100.0 * pingFails_ / pingCount_)
        : 0.0;
    if (ok) {
        dynamic_.pingMs = ms;
    }
}

void NetworkMonitor::ResolveStaticAdapterInfo() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    // Reuse the retained buffer and let ERROR_BUFFER_OVERFLOW drive growth; the
    // separate sizing call would repeat the full adapter enumeration.
    if (!gaaBuf_) {
        gaaBuf_ = static_cast<unsigned char*>(
            HeapAlloc(GetProcessHeap(), 0, kGaaInitialBytes));
        if (!gaaBuf_) {
            return;
        }
        gaaCapacity_ = kGaaInitialBytes;
    }
    ULONG size = gaaCapacity_;
    auto* buf = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(gaaBuf_);
    DWORD ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, buf, &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        auto* grown = static_cast<unsigned char*>(
            HeapReAlloc(GetProcessHeap(), 0, gaaBuf_, size));
        if (!grown) {
            return;
        }
        gaaBuf_ = grown;
        gaaCapacity_ = size;
        buf = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(gaaBuf_);
        ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, buf, &size);
    }
    if (ret != ERROR_SUCCESS) {
        return;
    }
    IP_ADAPTER_ADDRESSES* best = nullptr;
    IP_ADAPTER_ADDRESSES* bestWifi = nullptr;
    for (auto* a = buf; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) {
            best = a;
            break;
        }
        if (a->IfType == IF_TYPE_IEEE80211 && !bestWifi) {
            bestWifi = a;
        } else if (!best && IsEthernetLike(a->IfType)) {
            best = a;
        }
    }
    if (!best) {
        best = bestWifi;
    }
    NetworkStaticState localStatic{};
    NetworkDynamicState localDyn{};
    {
        Guard guard(&lock_);
        localDyn = dynamic_;
        localStatic.generation = static_.generation;
    }
    if (!best) {
        localStatic.adapterValid = false;
        localStatic.ifIndex = 0;
        wcscpy_s(localStatic.adapterName, L"No adapter");
        wcscpy_s(localStatic.ipAddress, L"—");
        wcscpy_s(localStatic.frequency, L"—");
        wcscpy_s(localStatic.linkSpeedLabel, L"—");
        localDyn.connected = false;
        haveCounters_ = false;
        Guard guard(&lock_);
        localStatic.generation = StaticLabelsDiffer(static_, localStatic)
            ? static_.generation + 1
            : static_.generation;
        static_ = localStatic;
        dynamic_.connected = false;
        return;
    }
    localStatic.adapterValid = true;
    localStatic.ifIndex = best->IfIndex;
    localStatic.luid = best->Luid;
    // AdapterName is "{GUID}"
    if (best->AdapterName) {
        wchar_t wguid[64]{};
        MultiByteToWideChar(CP_ACP, 0, best->AdapterName, -1, wguid, 64);
        GUID g{};
        if (SUCCEEDED(CLSIDFromString(wguid, &g))) {
            localStatic.interfaceGuid = g;
        }
    }
    if (best->FriendlyName) {
        wcsncpy_s(localStatic.adapterName, best->FriendlyName, _TRUNCATE);
        if (wcsstr(best->FriendlyName, L"Ethernet") || wcsstr(best->FriendlyName, L"ethernet")) {
            wcscpy_s(localStatic.adapterName, L"Ethernet");
        } else if (best->IfType == IF_TYPE_IEEE80211) {
            wcscpy_s(localStatic.adapterName, L"Wi-Fi");
        }
    } else {
        wcscpy_s(localStatic.adapterName, L"Ethernet");
    }
    // IPv4 unicast
    wcscpy_s(localStatic.ipAddress, L"—");
    for (auto* u = best->FirstUnicastAddress; u; u = u->Next) {
        if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            InetNtopW(AF_INET, &sin->sin_addr, localStatic.ipAddress, 64);
            break;
        }
    }
    // Wi-Fi band (2.4 / 5 / 6 GHz); wired shows —
    wcscpy_s(localStatic.frequency, L"—");
    if (best->IfType == IF_TYPE_IEEE80211) {
        // The BSS query needs a forced scan to return anything, so collapse the
        // interface-change bursts that drive this path onto one query per TTL.
        const ULONGLONG nowTick = GetTickCount64();
        const bool sameInterface = haveBand_ &&
            memcmp(&bandGuid_, &localStatic.interfaceGuid, sizeof(GUID)) == 0;
        if (!sameInterface || nowTick - bandQueryTick_ >= kBandTtlMs) {
            QueryWifiFrequency(wlanHandle_, localStatic.interfaceGuid, bandLabel_, 32);
            bandGuid_ = localStatic.interfaceGuid;
            bandQueryTick_ = nowTick;
            haveBand_ = true;
        }
        wcscpy_s(localStatic.frequency, bandLabel_);
    }
    MIB_IF_ROW2 ifRow{};
    ifRow.InterfaceLuid = localStatic.luid;
    if (GetIfEntry2(&ifRow) == NO_ERROR) {
        FormatLinkSpeed(ifRow.TransmitLinkSpeed, localStatic.linkSpeedLabel, 32);
        lastInOctets_ = ifRow.InOctets;
        lastOutOctets_ = ifRow.OutOctets;
        lastCounterTick_ = GetTickCount64();
        haveCounters_ = true;
        const bool sameSession = haveSessionBaseline_ &&
            sessionLuid_.Value == localStatic.luid.Value;
        if (!sameSession) {
            baselineIn_ = ifRow.InOctets;
            baselineOut_ = ifRow.OutOctets;
            sessionLuid_ = localStatic.luid;
            haveSessionBaseline_ = true;
            localDyn.downloadedBytes = 0;
            localDyn.uploadedBytes = 0;
            localDyn.recvBps = 0.0;
            localDyn.sendBps = 0.0;
        } else {
            localDyn.downloadedBytes = ifRow.InOctets - baselineIn_;
            localDyn.uploadedBytes = ifRow.OutOctets - baselineOut_;
        }
        localDyn.connected = (ifRow.OperStatus == IfOperStatusUp);
    } else {
        wcscpy_s(localStatic.linkSpeedLabel, L"—");
        localDyn.connected = true;
        haveCounters_ = false;
    }
    {
        Guard guard(&lock_);
        // Preserve ping / speed-test results across adapter re-resolve.
        localDyn.pingMs = dynamic_.pingMs;
        localDyn.packetLossPct = dynamic_.packetLossPct;
        localDyn.downloadMbps = dynamic_.downloadMbps;
        localDyn.uploadMbps = dynamic_.uploadMbps;
        localDyn.speedTestRunning = dynamic_.speedTestRunning;
        localStatic.generation = StaticLabelsDiffer(static_, localStatic)
            ? static_.generation + 1
            : static_.generation;
        static_ = localStatic;
        dynamic_ = localDyn;
    }
}

void NetworkMonitor::PollDynamicStats() {
    NET_LUID luid{};
    bool valid = false;
    {
        Guard guard(&lock_);
        valid = static_.adapterValid;
        luid = static_.luid;
    }
    if (!valid) {
        return;
    }
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    if (GetIfEntry2(&row) != NO_ERROR) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    Guard guard(&lock_);
    if (!haveCounters_) {
        lastInOctets_ = row.InOctets;
        lastOutOctets_ = row.OutOctets;
        lastCounterTick_ = now;
        const bool sameSession = haveSessionBaseline_ &&
            sessionLuid_.Value == luid.Value;
        if (!sameSession) {
            baselineIn_ = row.InOctets;
            baselineOut_ = row.OutOctets;
            sessionLuid_ = luid;
            haveSessionBaseline_ = true;
            dynamic_.downloadedBytes = 0;
            dynamic_.uploadedBytes = 0;
        }
        haveCounters_ = true;
    } else {
        const double dt = (now - lastCounterTick_) / 1000.0;
        if (dt > 0.05) {
            const ULONG64 dIn = row.InOctets - lastInOctets_;
            const ULONG64 dOut = row.OutOctets - lastOutOctets_;
            dynamic_.recvBps = static_cast<double>(dIn) / dt;
            dynamic_.sendBps = static_cast<double>(dOut) / dt;
            lastInOctets_ = row.InOctets;
            lastOutOctets_ = row.OutOctets;
            lastCounterTick_ = now;
        }
        dynamic_.downloadedBytes = row.InOctets - baselineIn_;
        dynamic_.uploadedBytes = row.OutOctets - baselineOut_;
    }
    dynamic_.connected = (row.OperStatus == IfOperStatusUp);
}

bool NetworkMonitor::PingOnce(double& outMs) {
    IN_ADDR addr{};
    HANDLE icmp = INVALID_HANDLE_VALUE;
    {
        Guard guard(&lock_);
        if (!pingAddrValid_.load(std::memory_order_relaxed)) {
            return false;
        }
        addr = pingAddr_;
        icmp = icmpHandle_;
    }
    if (icmp == INVALID_HANDLE_VALUE) {
        return false;
    }
    BYTE sendData[32] = {};
    constexpr DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 16;
    BYTE replyBuf[replySize] = {};
    const DWORD replied = IcmpSendEcho(
        icmp,
        addr.S_un.S_addr,
        sendData,
        sizeof(sendData),
        nullptr,
        replyBuf,
        replySize,
        1000);
    if (replied > 0) {
        auto* echo = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuf);
        if (echo->Status == IP_SUCCESS) {
            outMs = static_cast<double>(echo->RoundTripTime);
            return true;
        }
    }
    return false;
}
