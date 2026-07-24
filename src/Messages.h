#pragma once
#include "WinIncludes.h"
#include <cstdint>
// Custom messages shared across modules
inline constexpr UINT WM_APP_STATS_UPDATED   = WM_APP + 1;
inline constexpr UINT WM_APP_SPEED_DONE      = WM_APP + 2;
inline constexpr UINT WM_APP_TRAY            = WM_APP + 10;
inline constexpr UINT WM_APP_HIDE_POPUP      = WM_APP + 12;
// Static-until-network-change adapter identity / labels.
struct NetworkStaticState {
    bool adapterValid = false;
    ULONG ifIndex = 0;
    NET_LUID luid{};
    GUID interfaceGuid{};
    wchar_t adapterName[128]{};
    wchar_t ipAddress[64]{};
    wchar_t frequency[32]{};        // e.g. "2.4 GHz" / "5 GHz" / "6 GHz"
    wchar_t linkSpeedLabel[32]{};   // e.g. "5gbit"
    uint32_t generation = 0;
};
// 1 Hz + on-demand counters / latency / speed-test.
struct NetworkDynamicState {
    bool connected = false;
    double pingMs = -1.0;
    double packetLossPct = 0.0;
    double recvBps = 0.0;
    double sendBps = 0.0;
    ULONG64 downloadedBytes = 0;
    ULONG64 uploadedBytes = 0;
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    bool speedTestRunning = false;
};
struct NetworkSnapshot {
    bool connected = false;
    bool adapterValid = false;
    ULONG ifIndex = 0;
    NET_LUID luid{};
    GUID interfaceGuid{};
    wchar_t adapterName[128]{};
    wchar_t ipAddress[64]{};
    wchar_t frequency[32]{};
    wchar_t linkSpeedLabel[32]{};
    uint32_t staticGeneration = 0;
    double pingMs = -1.0;
    double packetLossPct = 0.0;
    double recvBps = 0.0;
    double sendBps = 0.0;
    ULONG64 downloadedBytes = 0;
    ULONG64 uploadedBytes = 0;
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    bool speedTestRunning = false;
};
inline NetworkSnapshot MergeSnapshot(const NetworkStaticState& st,
                                     const NetworkDynamicState& dyn) {
    NetworkSnapshot s{};
    s.connected = dyn.connected;
    s.adapterValid = st.adapterValid;
    s.ifIndex = st.ifIndex;
    s.luid = st.luid;
    s.interfaceGuid = st.interfaceGuid;
    wcsncpy_s(s.adapterName, st.adapterName, _TRUNCATE);
    wcsncpy_s(s.ipAddress, st.ipAddress, _TRUNCATE);
    wcsncpy_s(s.frequency, st.frequency, _TRUNCATE);
    wcsncpy_s(s.linkSpeedLabel, st.linkSpeedLabel, _TRUNCATE);
    s.staticGeneration = st.generation;
    s.pingMs = dyn.pingMs;
    s.packetLossPct = dyn.packetLossPct;
    s.recvBps = dyn.recvBps;
    s.sendBps = dyn.sendBps;
    s.downloadedBytes = dyn.downloadedBytes;
    s.uploadedBytes = dyn.uploadedBytes;
    s.downloadMbps = dyn.downloadMbps;
    s.uploadMbps = dyn.uploadMbps;
    s.speedTestRunning = dyn.speedTestRunning;
    return s;
}
// Fixed-capacity message buffer used for failure text across module boundaries.
// Keeps std::wstring (and with it the C++ runtime DLL) out of the binary.
struct ErrorMsg {
    wchar_t text[192]{};
    void Set(const wchar_t* s) { wcsncpy_s(text, s, _TRUNCATE); }
    void Clear() { text[0] = L'\0'; }
    bool Empty() const { return text[0] == L'\0'; }
};
enum class DnsProvider {
    Dhcp = 0,
    Cloudflare,
    Google,
    Custom,
};
struct AppConfig {
    DnsProvider dnsProvider = DnsProvider::Dhcp;
    wchar_t customDns[128]{};
    wchar_t pingTarget[64]{L"1.1.1.1"};
};
