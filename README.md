# Auto Hack 3in1

A Windows C++/Win32 helper for three GTA Online hacking minigames:

- Slider
- Signal repeater / flashing grid
- Fingerprint

The app detects the active supported minigame, shows a small HUD/overlay, and uses screen analysis plus simulated key input to complete the detected sequence.

## Requirements

- Windows
- MinGW-w64 with `g++.exe`, or MSVC Build Tools / Visual Studio with `cl.exe`
- PowerShell

For MinGW-w64, make sure `g++.exe` is available on `PATH`, or use the common `C:\mingw64\bin\g++.exe` fallback shown below.
For MSVC, run the command from a "Developer PowerShell for VS" prompt, or call `VsDevCmd.bat` first.

## Build

### MinGW-w64

```powershell
$root = (Get-Location).Path
$gxx = "g++"
if (-not (Get-Command $gxx -ErrorAction SilentlyContinue)) {
  $gxx = "C:\mingw64\bin\g++.exe"
}

& $gxx -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -municode -mwindows `
  "$root\src\main.cpp" `
  "$root\src\games\slider_module.cpp" `
  "$root\src\games\flashing_module.cpp" `
  "$root\src\games\fingerprint_module.cpp" `
  -lgdi32 -luser32 -lshell32 -lgdiplus -lcomctl32 -ldwmapi `
  -o "$root\auto_hack_3in1.exe"
```

### MSVC

```powershell
$root = (Get-Location).Path

cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN `
  "$root\src\main.cpp" `
  "$root\src\games\slider_module.cpp" `
  "$root\src\games\flashing_module.cpp" `
  "$root\src\games\fingerprint_module.cpp" `
  /link /SUBSYSTEM:WINDOWS `
  /OUT:"$root\auto_hack_3in1.exe" `
  user32.lib gdi32.lib shell32.lib gdiplus.lib comctl32.lib dwmapi.lib
```

The compiled executable is written to:

```text
auto_hack_3in1.exe
```

## Configuration

Runtime settings are stored next to the executable in `setting.ini`. The program creates and updates this file automatically, so you do not need to prepare it before running the app.

`setting.ini` is intentionally ignored by Git because it is local machine/user state. `setting.example.ini` documents the default values:

```ini
hotkey_vk=119
overlay_cursor=1
tap_hold_ms=8
tap_gap_ms=18
```

`hotkey_vk=119` is `F8`.

## Usage

1. Build and run `auto_hack_3in1.exe`.
2. Start the target game and open a supported hacking minigame.
3. Press the configured hotkey to start or stop detection/automation.
4. Use the taskbar button or HUD to bring the overlay back if needed.

## Repository Notes

Generated binaries, local build scripts, logs, and local settings are excluded from version control. Commit the source files, README, example config, `.gitignore`, and `.gitattributes`.
