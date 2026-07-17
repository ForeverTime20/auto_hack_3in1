# Auto Hack 3in1

A Windows C++/Win32 helper for three GTA Online hacking minigames:

- Slider
- Signal repeater / flashing grid
- Fingerprint

The app detects the active supported minigame, shows a small HUD/overlay, and uses screen analysis plus simulated key input to complete the detected sequence.

## Requirements

- Windows
- MinGW-w64 with `g++.exe`
- PowerShell

Make sure `g++.exe` is available on `PATH`, or replace `g++` in the command below with the full compiler path.

## Build

```powershell
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -municode -mwindows `
  src\main.cpp `
  src\games\slider_module.cpp `
  src\games\flashing_module.cpp `
  src\games\fingerprint_module.cpp `
  -lgdi32 -luser32 -lshell32 -lcomctl32 -ldwmapi `
  -o auto_hack_3in1.exe
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
4. Use the tray icon or HUD to bring the overlay back if needed.

## Repository Notes

Generated binaries, local build scripts, logs, and local settings are excluded from version control. Commit the source files, README, example config, `.gitignore`, and `.gitattributes`.
