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

By default, `build.ps1` looks for `C:\mingw64\bin\g++.exe`, then falls back to `g++.exe` on `PATH`. You can also set `CXX` to a compiler path:

```powershell
$env:CXX = "C:\path\to\g++.exe"
.\build.ps1
```

## Build

```powershell
.\build.ps1
```

The compiled executable is written to:

```text
auto_hack_3in1.exe
```

## Configuration

Runtime settings are stored next to the executable in `setting.ini`. This file is intentionally ignored by Git because it is local machine/user state.

To start from the default values, copy or recreate the values from `setting.example.ini`:

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

Generated binaries, logs, and local settings are excluded from version control. Commit the source files, build script, README, example config, `.gitignore`, and `.gitattributes`.
