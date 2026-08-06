# Adding a Minigame Module

This repository is a Windows C++17/Win32 application. Follow this contract when
adding another GTA hacking minigame. Preserve the ownership boundaries and
runtime behavior used by the existing five modules.

## Architecture

The application boundary owns:

- hotkey start/stop and the worker thread;
- GTA process/window discovery;
- the 20-second game/minigame discovery deadline;
- choosing exactly one module and running one session;
- global input timing configuration and final `CancelAll()`;
- creation/destruction of Win32 overlay windows;
- global overlay hiding, ingame-cache reset, and game-window cleanup;
- user settings, localization, HUD state, and status/log delivery.

A game module owns:

- its full-frame ingame signature and cached ingame geometry;
- lightweight validation of cached geometry;
- all game-specific ROI detection and dependent geometry;
- the analysis cadence and solver state machine;
- queued input plans and completion/result detection;
- overlay data and painting, when the game needs an overlay;
- cleanup of every module-owned cache and transient state.

Do not move game-specific pixel rules into `main.cpp`, `app/`, or
`capture/game_window.*`. Do not make `main.cpp` understand a module's solver
states. Conversely, modules must not read UI controls/settings directly when a
callback or configured common service already provides the value.

## Required Module API

Declare the module in `src/games/games.h`, under a dedicated namespace such as
`gta5::games::new_game`.

Every module must expose:

```cpp
bool DetectInGame();
void ResetInGameCache();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status);
```

The exact `RunSession` signature may additionally accept `overlayEnabled` and
`log` callbacks when needed. Slider predates this shape and reads the shared
runtime directly; new modules should prefer injected callbacks.

An overlay module normally also exposes:

```cpp
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();  // or HideOverlay(), but use one consistent name
LRESULT CALLBACK OverlayWindowProc(HWND, UINT, WPARAM, LPARAM);
```

`DetectInGame()` may be called repeatedly while the application searches for a
supported game. It must have no input side effects and must clear its pending
cache when detection fails. On success, it must retain the geometry found in
that same frame so `RunSession()` can consume it without doing an unconditional
second full-frame search.

`ResetInGameCache()` must be idempotent and cheap. It is called before every new
hotkey-started worker and again during worker teardown. `RunSession()` must also
clear module-owned caches on every return path, including capture failure,
normal completion, and stop requests.

`RunSession()` returns true only after the module has completed at least one
round/level or otherwise reached its defined successful terminal state. It must
poll `stopRequested()` frequently and stop scheduling input promptly.

## Ingame Detection Contract

Keep ingame detection separate from dynamic target analysis. Ingame detection
answers "is this module's minigame still present, and where are its stable
anchors?" It must follow this sequence:

1. The first successful full-frame search stores stable anchors and every
   downstream ROI that can be derived once from them.
2. `RunSession()` consumes that cached result.
3. Every analysis frame performs a lightweight validation at the known anchor
   coordinates. Sample small rectangles, lines, colors, or textures; do not
   rebuild a full-frame mask or connected-component graph on a cache hit.
4. If local validation fails, run the original full-frame search immediately in
   the same analysis iteration and on the same captured frame where possible.
   A module that normally captures only a cropped ROI must immediately capture
   a full frame before continuing; it must not wait for the next timer tick.
5. A successful relocation atomically replaces the ingame anchors and all
   geometry derived from them. Rebuild plans/maps that depend on changed
   coordinates.
6. A failed full search may feed a consecutive-miss/exit tolerance. The
   tolerance must not suppress the immediate full-search fallback.

Run the lightweight check on every module analysis frame. Do not add a separate
ingame timer or `Sleep` solely to throttle it. Reuse a full frame already needed
by the solver. The module may still control its overall analysis cadence for
gameplay reasons.

Current examples:

- Slider validates cached red-line/white-bar geometry and only then falls back
  to full `AnalyzeFrame`.
- Flashing validates three cached title bars and reuses the full frame already
  captured for level detection.
- Choose Fingerprint samples cached title regions instead of rebuilding its
  white mask and connected regions.
- Sort Fingerprint uses `anchorsPresent()` before `findHeaderBars()`.
- Fleeca validates the two cached connector boxes before `find_connectors()`.

Do not confuse a solver-result cache with an ingame-geometry cache. They have
different invalidation rules and should normally be separate types.

## Capture And Coordinates

Use `gta5::capture::CaptureGameFrame()` from
`src/capture/game_window.h`. Do not add another desktop/window capture path.

`GameFrame` has two coordinate spaces:

- `screenX`, `screenY`, `screenW`, `screenH`: absolute screen rectangle of the
  captured source, which may be a cropped region;
- `width`, `height`, `bgra`: analysis bitmap coordinates. A GTA client taller
  than 1080 pixels is proportionally downscaled;
- `toScreenX`, `toScreenY`: conversion factors from analysis pixels to screen
  pixels;
- `clientWidth`, `clientHeight`: full GTA client dimensions, independent of a
  cropped source region;
- `windowGeneration`: identity/version of the cached GTA window geometry.

Store geometry in one clearly documented coordinate space. Convert explicitly:

```cpp
screenX = frame.screenX + analysisX * frame.toScreenX;
analysisX = (screenX - frame.screenX) / frame.toScreenX;
```

Round and clamp before indexing. Never index `bgra` with absolute screen
coordinates.

Every ingame cache must record `windowGeneration` and the analysis dimensions
used to create it. Invalidate the cache when:

- capture fails or the GTA window is lost/minimized;
- `windowGeneration` changes (HWND, position, or client rectangle changed);
- the full analysis bitmap dimensions change;
- the session stops or a new hotkey session starts.

The capture layer refreshes and versions the client rectangle. A module must
still clear its local and pending cache when it observes a mismatch or failure.

## Input

Use `gta5::input` from `src/input/key_input.h`; do not call `SendInput` directly
from a new module.

- Use `QueueSequence()` for normal plans.
- Use `QueueImmediate()` only for latency-sensitive, precisely scheduled input.
- Pass the expected foreground window when the module depends on focus staying
  unchanged.
- Keep the returned `Job`, observe completion without blocking, and do not queue
  duplicate plans while a job is pending.
- The app calls `CancelAll()` at the boundary, but the module must reset its own
  job/state during teardown.

Game modules must not override global tap hold/gap settings. Those are configured
once by `main.cpp`.

## Overlay And Threading

The main program creates overlay HWNDs because Silent mode must create no game
overlay windows. A module receives an HWND through `SetOverlayWindow()` and must
work when it is null.

- Never steal focus or activate the overlay.
- Respect the `overlayEnabled` callback on every show/update path.
- Keep paint handlers read-only with respect to solver state.
- Publish a small immutable/copyable overlay snapshot under a mutex or critical
  section; do analysis outside the lock.
- Hide and clear stale overlays on exit, capture loss, and cache invalidation.
- Do not create nested worker threads unless the game genuinely requires one.
  The application already runs `RunSession()` on its worker thread.
- Send UI updates through callbacks or posted messages; do not manipulate main
  UI controls synchronously from the worker.

An overlay is optional. Do not add one merely for consistency.

## Main Program Integration

Adding `new_game_module.cpp` requires all applicable integration edits:

1. Add declarations to `src/games/games.h`.
2. Add the source to `src/games/CMakeLists.txt` and the `$sources` array in
   `build.ps1`.
3. Add a `GameKind` value and localized display-name mapping in `main.cpp`.
4. Add `DetectInGame()` in `DetectGame()`. Detection order is precedence; place
   similar signatures carefully to avoid a broad detector shadowing a specific
   one.
5. Add `ResetInGameCache()` to `ResetAllInGameCaches()`.
6. Add the `RunSession()` dispatch case and callbacks.
7. If an overlay exists, update class registration, creation, destruction,
   handle assignment, and `HideAllGameOverlays()`.
8. Add visible strings to both `src/resources/lang/zh-CN.rc` and
   `src/resources/lang/en-US.rc`; do not hard-code new persistent UI text in
   `main.cpp`.
9. Update the supported-game list and build source lists in `README.md`.

Keep orchestration changes mechanical. If integration requires substantial
game-specific branching in `main.cpp`, move that behavior back into the module.

## Implementation Style

- Match the existing single-file module style unless splitting the module
  removes real complexity.
- Use C++17 and Win32-compatible types. Keep builds working in both MinGW-w64
  and MSVC; avoid compiler-specific extensions.
- Prefer module-local helpers and structs in an unnamed namespace.
- Separate stable geometry, dynamic solver state, automation state, and overlay
  state.
- Allocate reusable scratch buffers outside hot inner loops when profiling shows
  allocation cost matters.
- Scale thresholds from the captured client/analysis height. Do not assume a
  1920x1080 desktop or a client origin of `(0, 0)`.
- Use consecutive-frame confirmation for noisy exits or state transitions, but
  keep input scheduling non-blocking.
- Status text should describe the current state, not every frame. Avoid posting
  duplicate status/log messages from a hot loop.
- Preserve Legacy and Enhanced GTA executable support by using the shared
  capture layer.

## Completion Checklist

Before handing off a new module:

- full detection cannot be confused with any earlier detector in `DetectGame()`;
- first detection geometry is reused by `RunSession()`;
- cached ingame validation touches only bounded known regions;
- local failure triggers immediate full-frame relocation;
- dependent geometry is rebuilt after relocation;
- window generation, capture size, loss, stop, and restart invalidate caches;
- stop requests cancel progress without leaving keys, jobs, or overlays active;
- Resident and Silent modes both work;
- non-1080p and windowed coordinate conversion is correct;
- both resource languages are updated for user-visible text;
- `git diff --check` passes;
- `./build.ps1` completes successfully.

Do not treat a successful compile as sufficient for vision changes. When test
frames are available, exercise at least initial detection, cache-hit validation,
forced local-validation failure/full relocation, resolution change, window loss,
manual stop, and a clean second hotkey start.
