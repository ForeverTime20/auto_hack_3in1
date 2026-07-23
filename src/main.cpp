#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <propidl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "games/games.h"
#include "capture/game_window.h"
#include "resources/resource.h"

namespace {

constexpr UINT kMsgLog = WM_APP + 1;
constexpr UINT kMsgStatus = WM_APP + 2;
constexpr UINT kMsgWorkerDone = WM_APP + 3;

HWND g_host = nullptr;
HICON g_appIcon = nullptr;
HANDLE g_singleInstanceMutex = nullptr;

enum class GameKind {
  None,
  Slider,
  Flashing,
  ChooseFingerprint,
  SortFingerprint,
};

std::wstring GameName(GameKind game) {
  switch (game) {
    case GameKind::Slider:
      return L"slider";
    case GameKind::Flashing:
      return L"flashing";
    case GameKind::ChooseFingerprint:
      return L"choose_fingerprint";
    case GameKind::SortFingerprint:
      return L"sort_fingerprint";
    default:
      return L"none";
  }
}

bool OverlayEnabled() {
  return gta5::games::slider::OverlayEnabled();
}

void HideAllGameOverlays() {
  gta5::games::slider::HideTransientOverlays();
  gta5::games::flashing::HideOverlay();
  gta5::games::choose_fingerprint::ClearOverlay();
  gta5::games::sort_fingerprint::ClearOverlay();
}

GameKind DetectGame() {
  if (gta5::games::slider::DetectInGame()) return GameKind::Slider;
  if (gta5::games::flashing::DetectInGame()) return GameKind::Flashing;
  if (gta5::games::choose_fingerprint::DetectInGame()) return GameKind::ChooseFingerprint;
  if (gta5::games::sort_fingerprint::DetectInGame()) return GameKind::SortFingerprint;
  return GameKind::None;
}

void PostStatus(const std::wstring& text) {
  gta5::games::slider::PostModuleStatus(text);
}

void PostLog(const std::wstring& text) {
  gta5::games::slider::PostModuleLog(text);
}

HICON LoadAppIcon(HINSTANCE inst) {
  return LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
}

void ApplyWindowIcon(HWND hwnd) {
  if (!hwnd || !g_appIcon) return;
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
}

void WorkerMain() {
  using Clock = std::chrono::steady_clock;
  gta5::games::slider::PostModuleLog(L"start");
  gta5::games::slider::PostModuleStatus(L"searching GTA5 window");
  HideAllGameOverlays();

  const auto deadline = Clock::now() + std::chrono::seconds(20);
  bool completed = false;
  bool gameWindowFound = false;
  while (!gta5::games::slider::StopRequested() && Clock::now() < deadline) {
    if (!gameWindowFound && !gta5::capture::FindGameWindow()) {
      PostStatus(L"searching GTA5 window");
      Sleep(100);
      continue;
    }
    if (!gameWindowFound) {
      PostLog(L"found GTA5 client area");
      PostStatus(L"searching minigame");
      gameWindowFound = true;
    }
    GameKind game = DetectGame();
    if (game == GameKind::None) {
      Sleep(30);
      continue;
    }

    PostLog(L"detected " + GameName(game));
    PostStatus(L"running " + GameName(game));
    switch (game) {
      case GameKind::Slider:
        gta5::games::slider::RunSession();
        completed = true;
        break;
      case GameKind::Flashing:
        completed = gta5::games::flashing::RunSession(
            [] { return gta5::games::slider::StopRequested(); },
            [] { return gta5::games::slider::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::ChooseFingerprint:
        completed = gta5::games::choose_fingerprint::RunSession(
            [] { return gta5::games::slider::StopRequested(); },
            [] { return gta5::games::slider::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::SortFingerprint:
        completed = gta5::games::sort_fingerprint::RunSession(
            [] { return gta5::games::slider::StopRequested(); },
            [] { return gta5::games::slider::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); },
            [](const std::wstring& text) { PostLog(text); });
        break;
      default:
        break;
    }
    break;
  }

  if (!completed && !gta5::games::slider::StopRequested() && Clock::now() >= deadline) {
    PostLog(gameWindowFound ? L"timeout: no supported minigame detected in 20s"
                            : L"timeout: GTA5 window not found in 20s");
    PostStatus(gameWindowFound ? L"detect timeout; stopped" : L"GTA5 timeout; stopped");
  } else {
    PostStatus(completed ? L"completed; stopped" : L"stopped");
  }

  HideAllGameOverlays();
  gta5::capture::ClearGameWindow();
  gta5::games::slider::MarkRunning(false);
  PostMessageW(g_host, kMsgWorkerDone, 0, 0);
}

void StartWorker() {
  if (gta5::games::slider::Running()) return;
  gta5::games::slider::ResetStopFlag();
  gta5::games::slider::HideTransientOverlays();
  gta5::games::slider::MarkRunning(true);
  gta5::games::slider::UpdatePreviewRunning(true);
  gta5::games::slider::SetHudStatusText(L"starting");
  PostStatus(L"starting");
  gta5::games::slider::RepaintHud();
  gta5::games::slider::WorkerThread() = std::thread(WorkerMain);
  gta5::games::slider::RepaintHud();
}

void StopWorker() {
  if (!gta5::games::slider::Running()) return;
  gta5::games::slider::SetHudStatusText(L"stopping");
  PostStatus(L"stopping");
  gta5::games::slider::RepaintHud();
  gta5::games::slider::RequestStop();
  HideAllGameOverlays();
  auto& worker = gta5::games::slider::WorkerThread();
  if (worker.joinable()) worker.join();
  gta5::games::slider::MarkRunning(false);
  gta5::games::slider::UpdatePreviewRunning(false);
  PostStatus(L"stopped");
  gta5::games::slider::RepaintHud();
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      gta5::games::slider::ApplyHotkey(hwnd);
      return 0;
    case WM_HOTKEY:
      if (static_cast<int>(wp) == gta5::games::slider::HotkeyId()) {
        if (gta5::games::slider::IsListeningHotkey()) return 0;
        if (gta5::games::slider::Running()) StopWorker();
        else StartWorker();
        return 0;
      }
      return 0;
    case kMsgLog: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
      OutputDebugStringW(text->c_str());
      OutputDebugStringW(L"\n");
      gta5::games::slider::SetHudLogText(*text);
      gta5::games::slider::RepaintHud();
      return 0;
    }
    case kMsgStatus: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
      gta5::games::slider::SetHudStatusText(*text);
      gta5::games::slider::RepaintHud();
      return 0;
    }
    case kMsgWorkerDone: {
      gta5::games::slider::UpdatePreviewRunning(false);
      auto& worker = gta5::games::slider::WorkerThread();
      if (worker.joinable()) worker.detach();
      gta5::games::slider::RepaintHud();
      return 0;
    }
    case WM_CLOSE:
      StopWorker();
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      StopWorker();
      UnregisterHotKey(hwnd, gta5::games::slider::HotkeyId());
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClasses(HINSTANCE inst) {
  WNDCLASSW host{};
  host.lpfnWndProc = HostProc;
  host.hInstance = inst;
  host.hCursor = LoadCursor(nullptr, IDC_ARROW);
  host.hIcon = g_appIcon;
  host.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  host.lpszClassName = L"Gta3In1HostV2";
  RegisterClassW(&host);

  WNDCLASSW hud{};
  hud.lpfnWndProc = gta5::games::slider::HudProc;
  hud.hInstance = inst;
  hud.hCursor = LoadCursor(nullptr, IDC_ARROW);
  hud.hIcon = g_appIcon;
  hud.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  hud.lpszClassName = L"Gta3In1HudV2";
  RegisterClassW(&hud);

  WNDCLASSW cursor{};
  cursor.lpfnWndProc = gta5::games::slider::CursorWindowProc;
  cursor.hInstance = inst;
  cursor.hCursor = LoadCursor(nullptr, IDC_ARROW);
  cursor.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cursor.lpszClassName = L"Gta3In1CursorV2";
  RegisterClassW(&cursor);

  WNDCLASSW marks{};
  marks.lpfnWndProc = gta5::games::slider::MarksWindowProc;
  marks.hInstance = inst;
  marks.hCursor = LoadCursor(nullptr, IDC_ARROW);
  marks.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  marks.lpszClassName = L"Gta3In1MarksV2";
  RegisterClassW(&marks);

  WNDCLASSW flashing{};
  flashing.lpfnWndProc = gta5::games::flashing::OverlayWindowProc;
  flashing.hInstance = inst;
  flashing.hCursor = LoadCursor(nullptr, IDC_ARROW);
  flashing.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  flashing.lpszClassName = L"Gta3In1FlashingOverlayV2";
  RegisterClassW(&flashing);

  WNDCLASSW fingerprint{};
  fingerprint.lpfnWndProc = gta5::games::choose_fingerprint::OverlayWindowProc;
  fingerprint.hInstance = inst;
  fingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
  fingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  fingerprint.lpszClassName = L"Gta3In1ChooseFingerprintOverlayV2";
  RegisterClassW(&fingerprint);

  WNDCLASSW sortFingerprint{};
  sortFingerprint.lpfnWndProc = gta5::games::sort_fingerprint::OverlayWindowProc;
  sortFingerprint.hInstance = inst;
  sortFingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
  sortFingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  sortFingerprint.lpszClassName = L"Gta3In1SortFingerprintOverlayV2";
  RegisterClassW(&sortFingerprint);
}

bool CreateWindows(HINSTANCE inst) {
  g_host = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gta3In1HostV2", L"Auto Hack 3in1 Host",
                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  if (!g_host) return false;
  ApplyWindowIcon(g_host);
  gta5::games::slider::SetHostWindow(g_host);

  RECT hudRect = gta5::games::slider::InitialHudRect();
  HWND hud = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
                             L"Gta3In1HudV2", L"Auto Hack 3in1 HUD",
                             WS_POPUP, hudRect.left, hudRect.top,
                             gta5::games::slider::HudWidth(), gta5::games::slider::HudHeight(),
                             nullptr, nullptr, inst, nullptr);
  gta5::games::slider::SetHudWindow(hud);
  if (hud) {
    ApplyWindowIcon(hud);
    SetLayeredWindowAttributes(hud, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(hud, SW_SHOWNA);
  }

  HWND cursor = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"Gta3In1CursorV2", L"Auto Hack 3in1 Cursor",
                                WS_POPUP, hudRect.right + 12, hudRect.top,
                                gta5::games::slider::CursorSize(), gta5::games::slider::CursorSize(),
                                nullptr, nullptr, inst, nullptr);
  gta5::games::slider::SetCursorWindow(cursor);
  if (cursor) {
    SetLayeredWindowAttributes(cursor, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(cursor, SW_HIDE);
  }

  HWND marks = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"Gta3In1MarksV2", L"Auto Hack 3in1 Marks",
                               WS_POPUP, hudRect.right + 84, hudRect.top, 1, 1,
                               nullptr, nullptr, inst, nullptr);
  gta5::games::slider::SetMarksWindow(marks);
  if (marks) {
    SetLayeredWindowAttributes(marks, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(marks, SW_HIDE);
  }

  HWND flashing = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                  L"Gta3In1FlashingOverlayV2", L"Auto Hack 3in1 Flashing Overlay",
                                  WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                                  nullptr, nullptr, inst, nullptr);
  gta5::games::flashing::SetOverlayWindow(flashing);
  if (flashing) {
    SetLayeredWindowAttributes(flashing, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(flashing, SW_HIDE);
  }

  HWND fingerprint = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                     L"Gta3In1ChooseFingerprintOverlayV2", L"Auto Hack 3in1 Choose Fingerprint Overlay",
                                     WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                                     nullptr, nullptr, inst, nullptr);
  gta5::games::choose_fingerprint::SetOverlayWindow(fingerprint);
  if (fingerprint) {
    SetLayeredWindowAttributes(fingerprint, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(fingerprint, SW_HIDE);
  }

  const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  HWND sortFingerprint = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta3In1SortFingerprintOverlayV2", L"Auto Hack 3in1 Sort Fingerprint Overlay",
      WS_POPUP, virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
  gta5::games::sort_fingerprint::SetOverlayWindow(sortFingerprint);
  if (sortFingerprint) {
    SetLayeredWindowAttributes(sortFingerprint, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(sortFingerprint, SW_HIDE);
  }

  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g_appIcon = LoadAppIcon(inst);

  g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoHack3in1SingleInstance");
  if (!g_singleInstanceMutex) {
    MessageBoxW(nullptr, L"Failed to start Auto Hack 3in1.", L"Auto Hack 3in1", MB_ICONERROR | MB_OK);
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Auto Hack 3in1 is already running.", L"Auto Hack 3in1", MB_ICONINFORMATION | MB_OK);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
    return 0;
  }

  gta5::games::slider::LoadPersistentSettings();
  gta5::games::choose_fingerprint::SetUiThread();
  gta5::games::choose_fingerprint::InitStateLock();

  RegisterClasses(inst);
  if (!CreateWindows(inst)) {
    return 1;
  }

  PostLog(L"4-in-1 ready: press " + gta5::games::slider::HotkeyName() + L" to start/stop");
  PostStatus(L"4-in-1 idle");

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  gta5::games::choose_fingerprint::DeleteStateLock();
  if (g_singleInstanceMutex) {
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
  }
  return 0;
}
