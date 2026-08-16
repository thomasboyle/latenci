# Routing Crumbs

Native Win32 system-tray network flyout for Windows 10/11 (x64). Live ping, throughput, session totals, IP, Wi-Fi band, Cloudflare speed test, and DNS switching — rendered with Direct2D.

<img width="432" height="339" alt="Routing Crumbs flyout" src="https://github.com/user-attachments/assets/ffda9aaf-be21-4f53-b817-cfc05ee0c26e" />

## Features

- **Live link stats** — receive/send rates, session download/upload totals, ICMP ping and rolling packet-loss % (20-sample window)
- **Adapter identity** — active interface name, IPv4 address, negotiated link speed, Wi-Fi band (2.4 / 5 / 6 GHz when available)
- **Speed test** — background WinHTTP download/upload against Cloudflare speed endpoints; quick probe on launch, full run from the flyout or tray menu
- **DNS switching** — DHCP, Cloudflare (`1.1.1.1`), Google (`8.8.8.8`), or custom IPv4 servers via `SetInterfaceDnsSettings` (no `netsh`)
- **Tray UX** — hover or left-click to open the flyout; pin by clicking; drag to reposition; right-click for Speed Test, Settings, Run at startup, Exit
- **Autostart** — optional elevated Task Scheduler task so the app comes back after sign-in
- **Paper chrome UI** — Direct2D / DirectWrite panel with Pixelify Sans, grain texture, and cottagecore palette

## Requirements

- Windows 10 1809+ or Windows 11 (**x64**)
- Visual Studio 2019 / 2022 / 2026 with **Desktop development with C++**
- CMake **3.20+**

## Build

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

For VS 2022, use `-G "Visual Studio 17 2022"` instead. Visual Studio **2026 Insiders** is selected with the `Visual Studio 18 2026` generator (detect via `-prerelease` if needed).

Release binary:

```text
build\Release\RoutingCrumbs.exe
```

Or open the generated `build\RoutingCrumbs.sln` / `.slnx` in Visual Studio and build **Release | x64**.

## Installer

The project ships an [Inno Setup](https://jrsoftware.org/isinfo.php) script (free, open source). The installer puts the app in Program Files, adds Start Menu / optional desktop shortcuts, registers an uninstaller, and removes the autostart task on uninstall.

1. Install **Inno Setup 6** from [jrsoftware.org/isinfo.php](https://jrsoftware.org/isinfo.php) (no cost).
2. Build Release, then the installer target:

```bat
cmake --build build --config Release --target installer
```

Output:

```text
dist\RoutingCrumbs-Setup-1.0.0.exe
```

Without Inno Setup on PATH, compile the script manually:

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\RoutingCrumbs.iss
```

**SmartScreen:** Windows may warn on first download because the installer is not code-signed. Code signing certificates cost money; the installer itself is a standard Inno Setup package with a proper uninstall entry in Settings. Users can verify the source by building from this repository.

## Run

Launch `RoutingCrumbs.exe`. A tray icon appears in the notification area (UAC elevation is required — see below).

| Input | Action |
|-------|--------|
| Left-click tray | Open / pin the status flyout |
| Hover tray | Peek flyout (dismisses when the cursor leaves) |
| Right-click tray | Context menu |
| Flyout DNS buttons | Apply DHCP / Cloudflare / Google / Custom |
| Flyout speed button | Run a full speed test |

## Admin elevation (DNS)

The app embeds a `requireAdministrator` manifest. Windows prompts for UAC on launch. Declining UAC prevents the process from starting.

Elevation is required so DNS provider changes can call `SetInterfaceDnsSettings` directly against the active adapter. Chosen provider and custom servers persist under:

```text
HKCU\Software\RoutingCrumbs
```

Values include DNS provider, custom DNS list, and ping target (default `1.1.1.1`).

## Autostart

**Run at startup** (tray menu) registers or removes a Task Scheduler task that launches the elevated binary at logon. The menu item reflects the current task state.

## Architecture

| Module | Role |
|--------|------|
| `NetworkMonitor` | Cache adapter LUID / IP / gateway / link speed on `NotifyIpInterfaceChange`; 1 Hz `GetIfEntry2` + ICMP for rates and ping; WLAN API for band |
| `PopupWindow` | Direct2D / DirectWrite flyout, hit-testing, paper chrome, drag |
| `SpeedTest` | Background WinHTTP download / upload against Cloudflare speed endpoints |
| `DnsManager` | `SetInterfaceDnsSettings` + `HKCU` persistence |
| `TrayIcon` | `Shell_NotifyIcon` (`NOTIFYICON_VERSION_4`) |
| `Autostart` | Task Scheduler logon task for elevated startup |
| `Fonts` | Embedded Pixelify Sans via DirectWrite custom font collection |

Stats paint is data-driven: the UI invalidates only on `WM_APP_STATS_UPDATED` (1 Hz while the flyout is open), not on a free-running redraw timer. Ping parking gates the ICMP worker while the popup is hidden; the 1 Hz interface poll keeps running (posting to the hidden window is suppressed), so idle cost stays low.

## Layout

```text
src/           Win32 application sources
resources/     icon, manifest, RC, fonts
CMakeLists.txt MSVC Release tuned for size (/O1, /LTCG, CFG, CET)
```
