# Stadia ViGEm

Map a **Google Stadia controller** (USB or Bluetooth) to a virtual **Xbox 360 gamepad** on Windows using [ViGEmBus](https://github.com/ViGEm/ViGEmBus).

Runs in the system tray. HidHide hides the physical controller so games see only the virtual Xbox pad.

Fork of [walkco/stadia-vigem](https://github.com/walkco/stadia-vigem) with BLE support, stability fixes, and an installer.

## Features

| Mode | Input | Vibration |
|------|-------|-----------|
| USB | Yes | Yes |
| Bluetooth | Yes | **No** (Windows 11 OS limitation) |

Bluetooth rumble was tested extensively (HID, Win32 GATT, WinRT + Sparse Package) — blocked by `hidbthle.sys`. See project docs for details.

## Requirements

- Windows 10/11 x64
- [ViGEmBus](https://github.com/ViGEm/ViGEmBus) (installed automatically)
- [HidHide](https://github.com/nefarius/HidHide) (installed automatically; prevents duplicate controllers)

## Quick install (end user)

1. Download the latest **Release** zip or clone this repo after building.
2. Ensure `bin\stadia-vigem-x64.exe` exists (from release or `build_quick.bat`).
3. Right-click **PowerShell → Run as administrator**, then:

```powershell
cd path\to\StadiaEmu
powershell -ExecutionPolicy Bypass -File .\Install.ps1
```

4. Launch **Stadia ViGEm** from the Start Menu.
5. Connect your Stadia controller (USB or Bluetooth pairing in Windows Settings).
6. Reboot once if ViGEmBus or HidHide were installed for the first time.

Tray menu: device count, **Start with Windows**, **Refresh**, **Quit**. HidHide is managed automatically.

## Build from source

Requires **Visual Studio 2022** with C++ desktop workload and Windows 10/11 SDK.

```bat
git clone --recurse-submodules https://github.com/YOUR_USER/StadiaEmu.git
cd StadiaEmu
build_quick.bat
```

Output: `bin\stadia-vigem-x64.exe`

Full CMake-style build: `.\Build.ps1 -Architecture x64 -Configuration RELEASE`

## Debug log

When running, the app writes `debug.log` next to the executable (same folder as `stadia-vigem-x64.exe`).

## Project layout

```
libstadia/          Core HID + Stadia protocol + GATT helpers
stadia-vigem/       Tray application + ViGEm bridge
ViGEmClient/        Submodule (ViGEm client library)
Install.ps1         End-user installer (drivers + app)
build_quick.bat     Fast local build
Build.ps1           Full VS build script
```

## License

MIT — see [LICENSE](LICENSE). Based on [walkco/stadia-vigem](https://github.com/walkco/stadia-vigem) (Copyright (c) 2020 grayver).

## Credits

- [ViGEmBus](https://github.com/ViGEm/ViGEmBus) / [ViGEmClient](https://github.com/ViGEm/ViGEmClient)
- [HidHide](https://github.com/nefarius/HidHide) (Nefarius)
- Chromium / SDL Stadia controller references
