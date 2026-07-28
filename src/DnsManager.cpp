#include "DnsManager.h"

#include <cstdio>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kRegPath[] = L"Software\\Latenci";

// Four IPv4 literals plus separators; matches AppConfig::customDns capacity.
constexpr size_t kNameServerChars = 128;
constexpr int kMaxServers = 4;

bool WriteRegString(HKEY key, const wchar_t* name, const wchar_t* value) {
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value), bytes) == ERROR_SUCCESS;
}

bool ReadRegString(HKEY key, const wchar_t* name, wchar_t* out, DWORD outChars) {
    DWORD type = 0;
    DWORD bytes = outChars * sizeof(wchar_t);
    const LONG r = RegQueryValueExW(key, name, nullptr, &type,
                                    reinterpret_cast<BYTE*>(out), &bytes);
    return r == ERROR_SUCCESS && type == REG_SZ;
}

bool IsValidIpv4(const wchar_t* s) {
    IN_ADDR addr{};
    return InetPtonW(AF_INET, s, &addr) == 1;
}

bool ParseCustomDnsList(const wchar_t* input, wchar_t* normalized, size_t normalizedCount,
                        ErrorMsg& error) {
    normalized[0] = L'\0';
    if (!input || !*input) {
        error.Set(L"Enter at least one IPv4 address");
        return false;
    }

    wchar_t work[kNameServerChars];
    wcsncpy_s(work, input, _TRUNCATE);
    // Accept comma or semicolon separators alongside spaces.
    for (wchar_t* c = work; *c; ++c) {
        if (*c == L',' || *c == L';') {
            *c = L' ';
        }
    }

    int count = 0;
    wchar_t* cursor = work;
    while (*cursor) {
        while (*cursor == L' ') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }
        wchar_t* token = cursor;
        while (*cursor && *cursor != L' ') {
            ++cursor;
        }
        if (*cursor) {
            *cursor = L'\0';
            ++cursor;
        }
        if (count == kMaxServers) {
            error.Set(L"At most 4 DNS servers");
            return false;
        }
        if (!IsValidIpv4(token)) {
            swprintf_s(error.text, L"Invalid IPv4: %s", token);
            return false;
        }
        if (count > 0) {
            wcscat_s(normalized, normalizedCount, L" ");
        }
        wcscat_s(normalized, normalizedCount, token);
        ++count;
    }

    if (count == 0) {
        error.Set(L"Enter at least one IPv4 address");
        return false;
    }
    return true;
}

}  // namespace

bool DnsManager::LoadConfig(AppConfig& out) {
    out = AppConfig{};
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD provider = 0;
    DWORD size = sizeof(provider);
    if (RegQueryValueExW(key, L"DnsProvider", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&provider), &size) == ERROR_SUCCESS) {
        if (provider <= static_cast<DWORD>(DnsProvider::Custom)) {
            out.dnsProvider = static_cast<DnsProvider>(provider);
        }
    }
    ReadRegString(key, L"CustomDns", out.customDns, 128);
    ReadRegString(key, L"PingTarget", out.pingTarget, 64);
    RegCloseKey(key);
    return true;
}

bool DnsManager::SaveConfig(const AppConfig& cfg) {
    HKEY key = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, &disp) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD provider = static_cast<DWORD>(cfg.dnsProvider);
    RegSetValueExW(key, L"DnsProvider", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&provider), sizeof(provider));
    WriteRegString(key, L"CustomDns", cfg.customDns);
    WriteRegString(key, L"PingTarget", cfg.pingTarget);
    RegCloseKey(key);
    return true;
}

bool DnsManager::Apply(DnsProvider provider, const GUID& ifaceGuid,
                       const wchar_t* customServers, ErrorMsg& error) {
    // Mutable storage: DNS_INTERFACE_SETTINGS::NameServer is a PWSTR, and an
    // empty string clears static DNS (back to DHCP).
    wchar_t nameServer[kNameServerChars]{};
    switch (provider) {
    case DnsProvider::Dhcp:
        break;
    case DnsProvider::Cloudflare:
        wcscpy_s(nameServer, L"1.1.1.1 1.0.0.1");
        break;
    case DnsProvider::Google:
        wcscpy_s(nameServer, L"8.8.8.8 8.8.4.4");
        break;
    case DnsProvider::Custom:
        if (!ParseCustomDnsList(customServers, nameServer, kNameServerChars, error)) {
            return false;
        }
        break;
    }

    DNS_INTERFACE_SETTINGS settings{};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER;
    settings.NameServer = nameServer;

    const DWORD err = SetInterfaceDnsSettings(ifaceGuid, &settings);
    if (err != NO_ERROR) {
        swprintf_s(error.text, L"SetInterfaceDnsSettings failed (%lu)", err);
        return false;
    }
    return true;
}
