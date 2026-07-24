# Routing Crumbs

Native Win32 system-tray network flyout for Windows 10/11 (x64). Live ping, throughput, totals, IP, Wi-Fi band, speed test, and DNS switching — rendered with Direct2D.

## Requirements

- Windows 10 1809+ or Windows 11 (x64)
- Visual Studio 2019/2022 with **Desktop development with C++**
- CMake 3.20+

## Build

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

(For VS 2022, use `-G "Visual Studio 17 2022"` instead.)

Visual Studio **2026 Insiders** is detected via `-prerelease`; use the `Visual Studio 18 2026` generator.

The binary is at `build\Release\RoutingCrumbs.exe`.

Or open the generated `build\RoutingCrumbs.sln` in Visual Studio and build.

## Run

Launch `RoutingCrumbs.exe`. A tray icon appears in the notification area.

- **Left-click / hover** — open the status flyout
- **Right-click** — Run Speed Test, Settings, Exit

## Admin elevation (DNS)

The app embeds a `requireAdministrator` manifest. Windows will prompt for UAC on launch.

Elevation is required so DNS provider changes (`DHCP`, `Cloudflare`, `Google`, `Custom`) can call `SetInterfaceDnsSettings` directly against the active adapter — no `netsh` subprocess. If you decline UAC, the process will not start.

Chosen DNS provider (and custom servers) persist under `HKCU\Software\RoutingCrumbs`.

## Architecture

| Module | Role |
|--------|------|
| `NetworkMonitor` | Cache adapter LUID/IP/gateway/link-speed on `NotifyIpInterfaceChange`; 1 Hz `GetIfEntry2` + ICMP for rates/ping |
| `PopupWindow` | Direct2D/DirectWrite flyout, hit-testing, paper chrome |
| `SpeedTest` | Background WinHTTP download/upload against Cloudflare speed endpoints |
| `DnsManager` | `SetInterfaceDnsSettings` + registry persistence |
| `TrayIcon` | `Shell_NotifyIcon` (NOTIFYICON_VERSION_4) |

Stats paint is data-driven: the UI invalidates only on `WM_APP_STATS_UPDATED` (1 Hz), not on a free-running redraw timer.
