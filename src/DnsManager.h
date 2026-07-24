#pragma once

#include "Messages.h"

class DnsManager {
public:
    static bool LoadConfig(AppConfig& out);
    static bool SaveConfig(const AppConfig& cfg);

    // Apply DNS for the given interface GUID. Requires elevation.
    static bool Apply(DnsProvider provider, const GUID& ifaceGuid, const wchar_t* customServers, ErrorMsg& error);
};
