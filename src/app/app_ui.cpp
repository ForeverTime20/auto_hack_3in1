#include "app_ui.h"
#include "localization.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>

namespace gta5::app::ui {
namespace {

using gta5::app::l10n::Language;

std::wstring T(const char* key) { return gta5::app::l10n::Text(key); }

constexpr int kHotkeyToggleId = 2001;
constexpr UINT kModeChangedMessage = WM_APP + 20;
constexpr int kHudWidth = 320;
constexpr int kHudMiniWidth = 232;
constexpr int kHudMiniHeight = 44;
constexpr int kHudExpandedHeight = 328;
constexpr int kHudMargin = 18;
constexpr ULONGLONG kHudAutoCollapseMs = 5000;

constexpr int kSetupResident = 3101;
constexpr int kSetupSilent = 3102;
constexpr int kSetupFast = 3103;
constexpr int kSetupSlow = 3104;
constexpr int kSetupCustom = 3105;
constexpr int kSetupHold = 3106;
constexpr int kSetupGap = 3107;
constexpr int kSetupSave = 3108;
constexpr int kSetupCancel = 3109;
constexpr int kDelayHold = 3201;
constexpr int kDelayGap = 3202;

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

HWND g_hostWnd = nullptr;
HWND g_hudWnd = nullptr;
HWND g_toastWnd = nullptr;
HWND g_hintWnd = nullptr;
std::atomic<bool> g_repaintPending{false};
std::atomic<bool> g_running{false};
std::atomic<int> g_hotkeyVk{VK_F6};
std::atomic<int> g_pendingHotkeyVk{VK_F6};
std::atomic<int> g_tapHoldMs{20};
std::atomic<int> g_tapGapMs{20};
std::atomic<int> g_launchMode{static_cast<int>(LaunchMode::Resident)};
std::atomic<int> g_delayPreset{static_cast<int>(DelayPreset::Fast)};
std::atomic<bool> g_listeningHotkey{false};
std::atomic<bool> g_panelExpanded{true};
std::atomic<bool> g_overlayEnabled{true};
std::mutex g_textMutex;
std::wstring g_status = L"idle";
std::wstring g_lastLog;
bool g_setupComplete = false;
bool g_userPlaced = false;
POINT g_hudPos{0, 0};
bool g_dragging = false;
POINT g_dragOffset{0, 0};
ULONGLONG g_lastInteractionTick = 0;
bool g_setupAccepted = false;
bool g_toastEnabled = false;
HICON g_dialogIcon = nullptr;
LaunchMode g_setupMode = LaunchMode::Resident;
DelayPreset g_setupPreset = DelayPreset::Fast;
std::wstring g_setupError;
std::wstring g_noticeTitle;
std::wstring g_noticeMessage;
HBRUSH g_dialogEditBrush = nullptr;
std::wstring g_hintText;
int g_hoverTarget = 0;
bool g_trackingHudMouse = false;

int CurrentHeight() {
  return g_panelExpanded.load(std::memory_order_relaxed) ? kHudExpandedHeight : kHudMiniHeight;
}

int CurrentWidth() {
  return g_panelExpanded.load(std::memory_order_relaxed) ? kHudWidth : kHudMiniWidth;
}

std::wstring FileNameOnly(const wchar_t* path) {
  if (!path) return L"";
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* forward = wcsrchr(path, L'/');
  const wchar_t* base = std::max(slash ? slash + 1 : path, forward ? forward + 1 : path);
  return base;
}

bool ProcessExeNameEquals(DWORD processId, const wchar_t* exeName) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
  if (!process) return false;
  wchar_t imagePath[MAX_PATH * 4]{};
  DWORD size = static_cast<DWORD>(std::size(imagePath));
  const bool matches = QueryFullProcessImageNameW(process, 0, imagePath, &size) &&
                       lstrcmpiW(FileNameOnly(imagePath).c_str(), exeName) == 0;
  CloseHandle(process);
  return matches;
}

struct WindowSearch {
  const wchar_t* exeName = nullptr;
  HWND window = nullptr;
  RECT rect{};
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM parameter) {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  if (!IsWindowVisible(hwnd) || hwnd == g_hostWnd || hwnd == g_hudWnd || GetWindow(hwnd, GW_OWNER)) return TRUE;
  RECT rect{};
  if (!GetWindowRect(hwnd, &rect) || rect.right - rect.left < 100 || rect.bottom - rect.top < 100) return TRUE;
  DWORD processId = 0;
  GetWindowThreadProcessId(hwnd, &processId);
  if (processId && ProcessExeNameEquals(processId, search->exeName)) {
    search->window = hwnd;
    search->rect = rect;
    return FALSE;
  }
  return TRUE;
}

bool FindWindowRectByExe(const wchar_t* exeName, RECT& rect) {
  WindowSearch search{exeName};
  EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&search));
  if (!search.window) return false;
  rect = search.rect;
  return true;
}

std::wstring ExeSiblingPath(const wchar_t* fileName) {
  wchar_t path[MAX_PATH * 4]{};
  DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (n == 0 || n >= std::size(path)) return fileName;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) *(slash + 1) = L'\0';
  return std::wstring(path) + fileName;
}

bool IsValidHotkeyVk(int vk) {
  if (vk < 8 || vk > 254) return false;
  if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2) return false;
  return vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU;
}

int ClampTapMs(int value) { return std::clamp(value, 1, 250); }

std::wstring KeyName(int vk) {
  if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
  if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
  if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));
  UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  wchar_t name[64]{};
  if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) return name;
  return L"VK" + std::to_wstring(vk);
}

void ApplyDelayPreset(DelayPreset preset) {
  g_delayPreset.store(static_cast<int>(preset), std::memory_order_relaxed);
  if (preset == DelayPreset::Fast) {
    g_tapHoldMs.store(20, std::memory_order_relaxed);
    g_tapGapMs.store(20, std::memory_order_relaxed);
  } else if (preset == DelayPreset::Slow) {
    g_tapHoldMs.store(40, std::memory_order_relaxed);
    g_tapGapMs.store(40, std::memory_order_relaxed);
  }
}

void SaveSettings() {
  std::ofstream out(ExeSiblingPath(L"setting.ini").c_str(), std::ios::trunc);
  if (!out) return;
  out << "setup_complete=" << (g_setupComplete ? 1 : 0) << "\n";
  out << "launch_mode=" << g_launchMode.load(std::memory_order_relaxed) << "\n";
  out << "delay_preset=" << g_delayPreset.load(std::memory_order_relaxed) << "\n";
  out << "language=" << static_cast<int>(gta5::app::l10n::CurrentLanguage()) << "\n";
  out << "hotkey_vk=" << g_hotkeyVk.load(std::memory_order_relaxed) << "\n";
  out << "overlay_cursor=" << (g_overlayEnabled.load(std::memory_order_relaxed) ? 1 : 0) << "\n";
  out << "tap_hold_ms=" << g_tapHoldMs.load(std::memory_order_relaxed) << "\n";
  out << "tap_gap_ms=" << g_tapGapMs.load(std::memory_order_relaxed) << "\n";
}

void LoadSettings() {
  int hotkey = VK_F6;
  int overlay = 1;
  int tapHold = 20;
  int tapGap = 20;
  int launchMode = static_cast<int>(LaunchMode::Resident);
  int delayPreset = -1;
  int language = static_cast<int>(Language::Chinese);
  int setupComplete = 0;
  std::ifstream in(ExeSiblingPath(L"setting.ini").c_str());
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      try {
        const std::string key = line.substr(0, eq);
        const int value = std::stoi(line.substr(eq + 1));
        if (key == "setup_complete") setupComplete = value;
        else if (key == "launch_mode") launchMode = value;
        else if (key == "delay_preset") delayPreset = value;
        else if (key == "language") language = value;
        else if (key == "hotkey_vk") hotkey = value;
        else if (key == "overlay_cursor") overlay = value;
        else if (key == "tap_hold_ms") tapHold = value;
        else if (key == "tap_gap_ms") tapGap = value;
      } catch (...) {
      }
    }
  }

  if (!IsValidHotkeyVk(hotkey)) hotkey = VK_F6;
  if (overlay != 0 && overlay != 1) overlay = 1;
  if (launchMode < static_cast<int>(LaunchMode::Resident) ||
      launchMode > static_cast<int>(LaunchMode::Silent)) {
    launchMode = static_cast<int>(LaunchMode::Resident);
  }
  tapHold = ClampTapMs(tapHold);
  tapGap = ClampTapMs(tapGap);
  if (delayPreset < static_cast<int>(DelayPreset::Fast) ||
      delayPreset > static_cast<int>(DelayPreset::Custom)) {
    delayPreset = tapHold == 20 && tapGap == 20 ? static_cast<int>(DelayPreset::Fast)
                  : tapHold == 40 && tapGap == 40 ? static_cast<int>(DelayPreset::Slow)
                                                  : static_cast<int>(DelayPreset::Custom);
  }
  if (language < static_cast<int>(Language::Chinese) ||
      language > static_cast<int>(Language::English)) {
    language = static_cast<int>(Language::Chinese);
  }
  gta5::app::l10n::SetLanguage(static_cast<Language>(language));

  g_setupComplete = setupComplete == 1;
  g_hotkeyVk.store(hotkey, std::memory_order_relaxed);
  g_pendingHotkeyVk.store(hotkey, std::memory_order_relaxed);
  g_overlayEnabled.store(overlay != 0, std::memory_order_relaxed);
  g_tapHoldMs.store(tapHold, std::memory_order_relaxed);
  g_tapGapMs.store(tapGap, std::memory_order_relaxed);
  g_launchMode.store(launchMode, std::memory_order_relaxed);
  g_delayPreset.store(delayPreset, std::memory_order_relaxed);
  g_status = T("status.idle");
}

RECT VirtualDesktopRect() {
  return RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
              GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
              GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

RECT ClampRect(RECT panel) {
  const int width = panel.right - panel.left;
  const int height = panel.bottom - panel.top;
  MONITORINFO info{sizeof(info)};
  const HMONITOR monitor = MonitorFromRect(&panel, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(monitor, &info)) info.rcMonitor = VirtualDesktopRect();
  const RECT& desktop = info.rcMonitor;
  const int minLeft = desktop.left;
  const int minTop = desktop.top + kHudMargin;
  const int maxLeft = std::max(minLeft, static_cast<int>(desktop.right) - width);
  const int maxTop = std::max(minTop, static_cast<int>(desktop.bottom) - height - kHudMargin);
  panel.left = std::clamp(static_cast<int>(panel.left), minLeft, maxLeft);
  panel.top = std::clamp(static_cast<int>(panel.top), minTop, maxTop);
  panel.right = panel.left + width;
  panel.bottom = panel.top + height;
  return panel;
}

RECT PanelRect() { return RECT{0, 0, CurrentWidth(), CurrentHeight()}; }
RECT ExitRect(const RECT& p) { return RECT{p.right - 36, 10, p.right - 12, 34}; }
RECT CollapseRect(const RECT& p) { return RECT{p.right - 66, 10, p.right - 42, 34}; }
RECT HeaderRect(const RECT& p) { return RECT{0, 0, p.right, 46}; }
RECT ResidentRect() { return RECT{16, 78, 156, 106}; }
RECT SilentRect() { return RECT{164, 78, 304, 106}; }
RECT HotkeyRect() { return RECT{86, 124, 190, 150}; }
RECT HotkeyActionRect() { return RECT{198, 124, 304, 150}; }
RECT AdminActionRect() { return RECT{198, 160, 304, 186}; }
RECT FastRect() { return RECT{16, 216, 104, 244}; }
RECT SlowRect() { return RECT{112, 216, 200, 244}; }
RECT CustomRect() { return RECT{208, 216, 304, 244}; }
RECT HoldEditRect() { return RECT{66, 254, 120, 280}; }
RECT GapEditRect() { return RECT{212, 254, 266, 280}; }
RECT OverlayRect() { return RECT{18, 296, 38, 316}; }
RECT OverlayHintRect() { return RECT{16, 290, 194, 322}; }
RECT ChineseRect() { return RECT{202, 294, 248, 318}; }
RECT EnglishRect() { return RECT{256, 294, 304, 318}; }
bool Contains(RECT rect, POINT point) { return PtInRect(&rect, point) != FALSE; }

bool RestartAsAdministrator(HWND owner) {
  wchar_t executable[MAX_PATH * 4]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  if (length == 0 || length >= std::size(executable)) return false;

  const std::wstring parameters = L"--elevated-restart=" + std::to_wstring(GetCurrentProcessId());
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.hwnd = owner;
  execute.lpVerb = L"runas";
  execute.lpFile = executable;
  execute.lpParameters = parameters.c_str();
  execute.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&execute)) return false;

  if (g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
  return true;
}

bool GetHoverHint(POINT point, int& target, RECT& anchor, std::wstring& text) {
  struct Hint {
    int id;
    RECT rect;
    const char* key;
  };
  const Hint hints[] = {
      {1, ResidentRect(), "hint.resident"},
      {2, SilentRect(), "hint.silent"},
      {3, HotkeyActionRect(), "hint.hotkey"},
      {4, FastRect(), "hint.fast"},
      {5, SlowRect(), "hint.slow"},
      {6, CustomRect(), "hint.custom"},
      {7, OverlayHintRect(), "hint.overlay"},
      {8, ChineseRect(), "hint.language"},
      {9, EnglishRect(), "hint.language"},
  };
  for (const Hint& hint : hints) {
    if (Contains(hint.rect, point)) {
      target = hint.id;
      anchor = hint.rect;
      text = T(hint.key);
      return true;
    }
  }
  return false;
}

void Touch() { g_lastInteractionTick = GetTickCount64(); }

void ResizeHud(HWND hwnd) {
  if (!hwnd) return;
  RECT wr{};
  GetWindowRect(hwnd, &wr);
  g_hudPos = POINT{wr.left, wr.top};
  HWND placeAfter = SilentMode() ? HWND_NOTOPMOST : HWND_TOPMOST;
  SetWindowPos(hwnd, placeAfter, wr.left, wr.top, CurrentWidth(), CurrentHeight(),
               SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Collapse(HWND hwnd) {
  g_panelExpanded.store(false, std::memory_order_relaxed);
  g_listeningHotkey.store(false, std::memory_order_relaxed);
  ShowWindow(GetDlgItem(hwnd, kDelayHold), SW_HIDE);
  ShowWindow(GetDlgItem(hwnd, kDelayGap), SW_HIDE);
  ResizeHud(hwnd);
  Repaint();
}

void Expand(HWND hwnd) {
  g_panelExpanded.store(true, std::memory_order_relaxed);
  g_listeningHotkey.store(false, std::memory_order_relaxed);
  Touch();
  ShowWindow(GetDlgItem(hwnd, kDelayHold), SW_SHOW);
  ShowWindow(GetDlgItem(hwnd, kDelayGap), SW_SHOW);
  ResizeHud(hwnd);
  Repaint();
}

HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
  const wchar_t* face = gta5::app::l10n::CurrentLanguage() == Language::Chinese
                            ? L"Microsoft YaHei UI"
                            : L"Segoe UI";
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, face);
}

void DrawCenteredText(HDC hdc, RECT rect, const std::wstring& text, COLORREF color) {
  SetTextColor(hdc, color);
  DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawTextAt(HDC hdc, int x, int y, const std::wstring& text) {
  TextOutW(hdc, x, y, text.c_str(), static_cast<int>(text.size()));
}

void DrawSegment(HDC hdc, RECT rect, const std::wstring& text, bool selected,
                 HBRUSH selectedBrush, HBRUSH normalBrush, HPEN accentPen) {
  SelectObject(hdc, selected ? selectedBrush : normalBrush);
  SelectObject(hdc, accentPen);
  RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 7, 7);
  DrawCenteredText(hdc, rect, text, selected ? RGB(10, 20, 18) : RGB(205, 215, 225));
}

void DrawEditFrame(HDC hdc, RECT rect, bool enabled);

void DrawPanel(HDC hdc, const RECT& panel) {
  const bool expanded = g_panelExpanded.load(std::memory_order_relaxed);
  const bool running = g_running.load(std::memory_order_relaxed);
  std::wstring status;
  {
    std::lock_guard<std::mutex> lock(g_textMutex);
    status = g_status;
  }

  HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
  HPEN dangerPen = CreatePen(PS_SOLID, 2, RGB(240, 105, 105));
  HPEN disabledPen = CreatePen(PS_SOLID, 1, RGB(65, 75, 84));
  HBRUSH panelBrush = CreateSolidBrush(RGB(16, 20, 25));
  HBRUSH headerBrush = CreateSolidBrush(RGB(24, 31, 38));
  HBRUSH accentBrush = CreateSolidBrush(RGB(79, 220, 145));
  HBRUSH runningBrush = CreateSolidBrush(RGB(255, 177, 72));
  HBRUSH normalBrush = CreateSolidBrush(RGB(35, 43, 51));
  HBRUSH dangerBrush = CreateSolidBrush(RGB(47, 27, 31));
  HFONT font = CreateUiFont(22, FW_SEMIBOLD);
  HFONT boldFont = CreateUiFont(22, FW_BOLD);
  HFONT labelFont = CreateUiFont(20, FW_BOLD);
  HGDIOBJ oldFont = SelectObject(hdc, font);
  HGDIOBJ oldPen = SelectObject(hdc, accentPen);
  HGDIOBJ oldBrush = SelectObject(hdc, panelBrush);
  SetBkMode(hdc, TRANSPARENT);

  RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom, expanded ? 10 : 16, expanded ? 10 : 16);
  if (!expanded) {
    SelectObject(hdc, running ? runningBrush : accentBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, 14, 16, 26, 28);
    SelectObject(hdc, boldFont);
    RECT title{34, 0, 100, panel.bottom};
    DrawCenteredText(hdc, title, T(running ? "state.on" : "state.off"),
                     running ? RGB(255, 190, 90) : RGB(130, 240, 175));
    SelectObject(hdc, font);
    RECT text{96, 0, panel.right - 12, panel.bottom};
    DrawTextW(hdc, status.c_str(), static_cast<int>(std::min<size_t>(status.size(), 18)), &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else {
    SelectObject(hdc, headerBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, 0, 0, panel.right, 46, 10, 10);
    SelectObject(hdc, running ? runningBrush : accentBrush);
    Ellipse(hdc, 14, 17, 26, 29);
    SelectObject(hdc, boldFont);
    RECT stateRect{34, 4, 95, 42};
    DrawCenteredText(hdc, stateRect, T(running ? "state.on" : "state.off"),
                     running ? RGB(255, 190, 90) : RGB(130, 240, 175));
    SelectObject(hdc, font);
    RECT statusRect{92, 4, panel.right - 76, 42};
    SetTextColor(hdc, RGB(196, 207, 218));
    DrawTextW(hdc, status.c_str(), static_cast<int>(status.size()), &statusRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(hdc, normalBrush);
    SelectObject(hdc, accentPen);
    RECT collapse = CollapseRect(panel);
    RoundRect(hdc, collapse.left, collapse.top, collapse.right, collapse.bottom, 6, 6);
    MoveToEx(hdc, collapse.left + 6, collapse.top + 12, nullptr);
    LineTo(hdc, collapse.right - 6, collapse.top + 12);
    RECT exit = ExitRect(panel);
    SelectObject(hdc, dangerBrush);
    SelectObject(hdc, dangerPen);
    RoundRect(hdc, exit.left, exit.top, exit.right, exit.bottom, 6, 6);
    MoveToEx(hdc, exit.left + 7, exit.top + 7, nullptr);
    LineTo(hdc, exit.right - 7, exit.bottom - 7);
    MoveToEx(hdc, exit.right - 7, exit.top + 7, nullptr);
    LineTo(hdc, exit.left + 7, exit.bottom - 7);

    SelectObject(hdc, labelFont);
    SetTextColor(hdc, RGB(124, 139, 153));
    DrawTextAt(hdc, 16, 55, T("panel.mode"));
    const LaunchMode mode = CurrentLaunchMode();
    SelectObject(hdc, font);
    DrawSegment(hdc, ResidentRect(), T("mode.resident"), mode == LaunchMode::Resident,
                accentBrush, normalBrush, accentPen);
    DrawSegment(hdc, SilentRect(), T("mode.silent"), mode == LaunchMode::Silent,
                accentBrush, normalBrush, accentPen);

    SelectObject(hdc, labelFont);
    SetTextColor(hdc, RGB(183, 196, 208));
    DrawTextAt(hdc, 16, 128, T("panel.hotkey"));
    SelectObject(hdc, font);
    SelectObject(hdc, normalBrush);
    SelectObject(hdc, accentPen);
    const RECT hotkeyRect = HotkeyRect();
    RoundRect(hdc, hotkeyRect.left, hotkeyRect.top, hotkeyRect.right, hotkeyRect.bottom, 7, 7);
    const bool listening = g_listeningHotkey.load(std::memory_order_relaxed);
    DrawCenteredText(hdc, HotkeyRect(), KeyName(listening ? g_pendingHotkeyVk.load() : g_hotkeyVk.load()),
                     listening ? RGB(255, 211, 105) : RGB(225, 234, 242));
    DrawSegment(hdc, HotkeyActionRect(), T(listening ? "action.confirm" : "action.change"), false,
                accentBrush, normalBrush, accentPen);

    SetTextColor(hdc, RGB(183, 196, 208));
    DrawTextAt(hdc, 16, 164, T("admin.try_prompt"));
    DrawSegment(hdc, AdminActionRect(), T("admin.run"), false,
                accentBrush, normalBrush, accentPen);

    SelectObject(hdc, labelFont);
    SetTextColor(hdc, RGB(124, 139, 153));
    DrawTextAt(hdc, 16, 196, T("panel.key_delay"));
    const DelayPreset preset = static_cast<DelayPreset>(g_delayPreset.load(std::memory_order_relaxed));
    SelectObject(hdc, font);
    DrawSegment(hdc, FastRect(), T("delay.fast"), preset == DelayPreset::Fast,
                accentBrush, normalBrush, accentPen);
    DrawSegment(hdc, SlowRect(), T("delay.slow"), preset == DelayPreset::Slow,
                accentBrush, normalBrush, accentPen);
    DrawSegment(hdc, CustomRect(), T("delay.custom"), preset == DelayPreset::Custom,
                accentBrush, normalBrush, accentPen);

    SetTextColor(hdc, RGB(183, 196, 208));
    DrawTextAt(hdc, 16, 258, T("delay.hold"));
    DrawTextAt(hdc, 126, 258, T("unit.ms"));
    DrawTextAt(hdc, 164, 258, T("delay.gap"));
    DrawTextAt(hdc, 272, 258, T("unit.ms"));
    const bool customDelay = preset == DelayPreset::Custom;
    DrawEditFrame(hdc, HoldEditRect(), customDelay);
    DrawEditFrame(hdc, GapEditRect(), customDelay);

    const bool overlayAvailable = mode == LaunchMode::Resident;
    const bool overlayOn = overlayAvailable && g_overlayEnabled.load(std::memory_order_relaxed);
    SelectObject(hdc, overlayOn ? accentBrush : normalBrush);
    SelectObject(hdc, overlayAvailable ? accentPen : disabledPen);
    Rectangle(hdc, 18, 296, 38, 316);
    if (overlayOn) {
      MoveToEx(hdc, 22, 306, nullptr);
      LineTo(hdc, 27, 311);
      LineTo(hdc, 35, 300);
    }
    SetTextColor(hdc, overlayAvailable ? RGB(183, 196, 208) : RGB(103, 116, 128));
    DrawTextAt(hdc, 48, 298, T(overlayAvailable ? "overlay.show" : "overlay.silent"));
    DrawSegment(hdc, ChineseRect(), T("language.chinese"),
                gta5::app::l10n::CurrentLanguage() == Language::Chinese,
                accentBrush, normalBrush, accentPen);
    DrawSegment(hdc, EnglishRect(), T("language.english"),
                gta5::app::l10n::CurrentLanguage() == Language::English,
                accentBrush, normalBrush, accentPen);
  }

  SelectObject(hdc, oldFont);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(font);
  DeleteObject(boldFont);
  DeleteObject(labelFont);
  DeleteObject(accentPen);
  DeleteObject(dangerPen);
  DeleteObject(disabledPen);
  DeleteObject(panelBrush);
  DeleteObject(headerBrush);
  DeleteObject(accentBrush);
  DeleteObject(runningBrush);
  DeleteObject(normalBrush);
  DeleteObject(dangerBrush);
}

HWND AddDarkEdit(HWND parent, const std::wstring& value, int x, int y, int w, int h, int id) {
  HWND edit = CreateWindowExW(0, L"EDIT", value.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                              x, y, w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandleW(nullptr), nullptr);
  if (edit) {
    HFONT font = CreateUiFont(22, FW_SEMIBOLD);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(font));
    SendMessageW(edit, EM_SETLIMITTEXT, 3, 0);
  }
  return edit;
}

LRESULT CALLBACK HintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      HBRUSH background = CreateSolidBrush(RGB(24, 31, 38));
      HPEN border = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
      HGDIOBJ oldBrush = SelectObject(hdc, background);
      HGDIOBJ oldPen = SelectObject(hdc, border);
      RoundRect(hdc, 0, 0, client.right, client.bottom, 8, 8);
      HFONT font = CreateUiFont(18, FW_SEMIBOLD);
      HGDIOBJ oldFont = SelectObject(hdc, font);
      SetBkMode(hdc, TRANSPARENT);
      RECT textRect{12, 0, client.right - 12, client.bottom};
      DrawCenteredText(hdc, textRect, g_hintText, RGB(210, 221, 231));
      SelectObject(hdc, oldFont);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBrush);
      DeleteObject(font);
      DeleteObject(border);
      DeleteObject(background);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      if (g_hintWnd == hwnd) g_hintWnd = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void HideHoverHint() {
  g_hoverTarget = 0;
  if (g_hintWnd) ShowWindow(g_hintWnd, SW_HIDE);
}

void ShowHoverHint(HWND owner, int target, RECT anchor, const std::wstring& text) {
  if (target == g_hoverTarget && g_hintWnd && IsWindowVisible(g_hintWnd)) return;
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = HintProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"AutoHackHoverHintV1";
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered) return;

  constexpr int height = 42;
  SIZE textSize{};
  HDC measureDc = GetDC(owner);
  HFONT measureFont = CreateUiFont(18, FW_SEMIBOLD);
  HGDIOBJ oldMeasureFont = SelectObject(measureDc, measureFont);
  GetTextExtentPoint32W(measureDc, text.c_str(), static_cast<int>(text.size()), &textSize);
  SelectObject(measureDc, oldMeasureFont);
  DeleteObject(measureFont);
  ReleaseDC(owner, measureDc);
  RECT desktop = VirtualDesktopRect();
  const int maxWidth = std::max(120, static_cast<int>(desktop.right - desktop.left) - 24);
  const int width = std::clamp(static_cast<int>(textSize.cx) + 24, 120, maxWidth);
  if (!g_hintWnd) {
    g_hintWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                L"AutoHackHoverHintV1", L"", WS_POPUP,
                                0, 0, width, height, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g_hintWnd) return;
  }
  SetWindowRgn(g_hintWnd, CreateRoundRectRgn(0, 0, width, height, 8, 8), TRUE);

  POINT below{(anchor.left + anchor.right) / 2, anchor.bottom + 6};
  ClientToScreen(owner, &below);
  int x = std::clamp(static_cast<int>(below.x) - width / 2, static_cast<int>(desktop.left),
                     std::max(static_cast<int>(desktop.left), static_cast<int>(desktop.right) - width));
  int y = below.y;
  if (y + height > desktop.bottom) {
    POINT above{0, anchor.top - height - 6};
    ClientToScreen(owner, &above);
    y = above.y;
  }
  g_hintText = text;
  g_hoverTarget = target;
  InvalidateRect(g_hintWnd, nullptr, FALSE);
  SetWindowPos(g_hintWnd, SilentMode() ? HWND_TOP : HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void DestroyEditFont(HWND edit) {
  if (!edit) return;
  HFONT font = reinterpret_cast<HFONT>(GetWindowLongPtrW(edit, GWLP_USERDATA));
  if (font) DeleteObject(font);
}

bool ReadDelayEdits(HWND hwnd, int holdId, int gapId, int& hold, int& gap, std::wstring& error) {
  BOOL holdOk = FALSE;
  BOOL gapOk = FALSE;
  hold = static_cast<int>(GetDlgItemInt(hwnd, holdId, &holdOk, FALSE));
  gap = static_cast<int>(GetDlgItemInt(hwnd, gapId, &gapOk, FALSE));
  if (holdOk && gapOk && hold >= 1 && hold <= 250 && gap >= 1 && gap <= 250) {
    error.clear();
    return true;
  }
  error = T("validation.delay");
  InvalidateRect(hwnd, nullptr, FALSE);
  return false;
}

void SetEditValue(HWND edit, int value) {
  if (edit) SetWindowTextW(edit, std::to_wstring(value).c_str());
}

void SyncHudDelayControls(HWND hwnd) {
  const DelayPreset preset = static_cast<DelayPreset>(g_delayPreset.load(std::memory_order_relaxed));
  HWND holdEdit = GetDlgItem(hwnd, kDelayHold);
  HWND gapEdit = GetDlgItem(hwnd, kDelayGap);
  SetEditValue(holdEdit, TapHoldMs());
  SetEditValue(gapEdit, TapGapMs());
  const BOOL editable = preset == DelayPreset::Custom;
  EnableWindow(holdEdit, editable);
  EnableWindow(gapEdit, editable);
  InvalidateRect(hwnd, nullptr, FALSE);
}

void SaveHudCustomDelay(HWND hwnd) {
  if (static_cast<DelayPreset>(g_delayPreset.load(std::memory_order_relaxed)) != DelayPreset::Custom) {
    return;
  }
  int hold = 0;
  int gap = 0;
  std::wstring ignoredError;
  if (!ReadDelayEdits(hwnd, kDelayHold, kDelayGap, hold, gap, ignoredError)) return;
  g_tapHoldMs.store(hold, std::memory_order_relaxed);
  g_tapGapMs.store(gap, std::memory_order_relaxed);
  SaveSettings();
}

void DrawDialogBase(HDC hdc, const RECT& client, const std::wstring& title,
                    const std::wstring& subtitle, RECT closeRect) {
  HBRUSH panel = CreateSolidBrush(RGB(16, 20, 25));
  HBRUSH header = CreateSolidBrush(RGB(24, 31, 38));
  HBRUSH danger = CreateSolidBrush(RGB(47, 27, 31));
  HPEN accent = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
  HPEN dangerPen = CreatePen(PS_SOLID, 2, RGB(240, 105, 105));
  HFONT titleFont = CreateUiFont(29, FW_BOLD);
  HFONT bodyFont = CreateUiFont(21, FW_SEMIBOLD);
  HGDIOBJ oldBrush = SelectObject(hdc, panel);
  HGDIOBJ oldPen = SelectObject(hdc, accent);
  HGDIOBJ oldFont = SelectObject(hdc, titleFont);
  SetBkMode(hdc, TRANSPARENT);
  RoundRect(hdc, 0, 0, client.right, client.bottom, 12, 12);
  SelectObject(hdc, header);
  SelectObject(hdc, GetStockObject(NULL_PEN));
  const int headerHeight = subtitle.empty() ? 58 : 68;
  RoundRect(hdc, 0, 0, client.right, headerHeight, 12, 12);
  SetTextColor(hdc, RGB(224, 234, 242));
  TextOutW(hdc, 26, subtitle.empty() ? 16 : 12, title.c_str(), static_cast<int>(title.size()));
  if (!subtitle.empty()) {
    SelectObject(hdc, bodyFont);
    SetTextColor(hdc, RGB(132, 148, 162));
    TextOutW(hdc, 26, 43, subtitle.c_str(), static_cast<int>(subtitle.size()));
  }
  SelectObject(hdc, danger);
  SelectObject(hdc, dangerPen);
  RoundRect(hdc, closeRect.left, closeRect.top, closeRect.right, closeRect.bottom, 6, 6);
  MoveToEx(hdc, closeRect.left + 7, closeRect.top + 7, nullptr);
  LineTo(hdc, closeRect.right - 7, closeRect.bottom - 7);
  MoveToEx(hdc, closeRect.right - 7, closeRect.top + 7, nullptr);
  LineTo(hdc, closeRect.left + 7, closeRect.bottom - 7);
  SelectObject(hdc, oldFont);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(panel);
  DeleteObject(header);
  DeleteObject(danger);
  DeleteObject(accent);
  DeleteObject(dangerPen);
  DeleteObject(titleFont);
  DeleteObject(bodyFont);
}

void DrawEditFrame(HDC hdc, RECT rect, bool enabled) {
  HPEN pen = CreatePen(PS_SOLID, 1, enabled ? RGB(67, 205, 132) : RGB(65, 75, 84));
  HGDIOBJ oldPen = SelectObject(hdc, pen);
  HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
  RoundRect(hdc, rect.left - 1, rect.top - 1, rect.right + 1, rect.bottom + 1, 6, 6);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

RECT SetupCloseRect() { return RECT{558, 15, 586, 43}; }
RECT SetupChineseRect() { return RECT{420, 15, 478, 43}; }
RECT SetupEnglishRect() { return RECT{484, 15, 542, 43}; }
RECT SetupResidentRect() { return RECT{162, 64, 360, 94}; }
RECT SetupSilentRect() { return RECT{372, 64, 580, 94}; }
RECT SetupFastRect() { return RECT{200, 210, 320, 240}; }
RECT SetupSlowRect() { return RECT{330, 210, 450, 240}; }
RECT SetupCustomRect() { return RECT{460, 210, 580, 240}; }
RECT SetupCancelRect() { return RECT{358, 338, 464, 368}; }
RECT SetupSaveRect() { return RECT{476, 338, 580, 368}; }

void SetSetupPreset(HWND hwnd, DelayPreset preset) {
  g_setupPreset = preset;
  const bool custom = preset == DelayPreset::Custom;
  EnableWindow(GetDlgItem(hwnd, kSetupHold), custom);
  EnableWindow(GetDlgItem(hwnd, kSetupGap), custom);
  g_setupError.clear();
  InvalidateRect(hwnd, nullptr, FALSE);
}

void DrawSetup(HWND hwnd, HDC hdc) {
  RECT client{};
  GetClientRect(hwnd, &client);
  DrawDialogBase(hdc, client, T("setup.title"), L"", SetupCloseRect());
  HFONT labelFont = CreateUiFont(20, FW_BOLD);
  HFONT bodyFont = CreateUiFont(22, FW_SEMIBOLD);
  HGDIOBJ oldFont = SelectObject(hdc, labelFont);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(125, 141, 155));
  DrawTextAt(hdc, 20, 70, T("setup.startup_mode"));
  HBRUSH accentBrush = CreateSolidBrush(RGB(79, 220, 145));
  HBRUSH normalBrush = CreateSolidBrush(RGB(35, 43, 51));
  HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
  SelectObject(hdc, bodyFont);
  DrawSegment(hdc, SetupChineseRect(), T("language.chinese"),
              gta5::app::l10n::CurrentLanguage() == Language::Chinese,
              accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupEnglishRect(), T("language.english"),
              gta5::app::l10n::CurrentLanguage() == Language::English,
              accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupResidentRect(), T("mode.resident"), g_setupMode == LaunchMode::Resident,
              accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupSilentRect(), T("mode.silent"), g_setupMode == LaunchMode::Silent,
              accentBrush, normalBrush, accentPen);
  HFONT detailFont = CreateUiFont(18, FW_SEMIBOLD);
  SelectObject(hdc, detailFont);
  SetTextColor(hdc, RGB(176, 190, 202));
  RECT residentCopy{24, 104, 576, 146};
  RECT silentCopy{24, 148, 576, 204};
  const std::wstring residentParagraph = T("setup.resident_desc");
  const std::wstring silentParagraph = T("setup.silent_desc");
  DrawTextW(hdc, residentParagraph.c_str(), -1, &residentCopy,
            DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
  DrawTextW(hdc, silentParagraph.c_str(), -1, &silentCopy,
            DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

  SelectObject(hdc, labelFont);
  SetTextColor(hdc, RGB(125, 141, 155));
  DrawTextAt(hdc, 20, 216, T("setup.default_delay"));
  SelectObject(hdc, bodyFont);
  DrawSegment(hdc, SetupFastRect(), T("delay.fast_values"), g_setupPreset == DelayPreset::Fast,
              accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupSlowRect(), T("delay.slow_values"), g_setupPreset == DelayPreset::Slow,
              accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupCustomRect(), T("delay.custom"), g_setupPreset == DelayPreset::Custom,
              accentBrush, normalBrush, accentPen);
  SelectObject(hdc, detailFont);
  RECT delayCopy{24, 246, 576, 304};
  SetTextColor(hdc, RGB(176, 190, 202));
  const std::wstring delayText = T("setup.delay_desc");
  if (g_setupError.empty()) {
    DrawTextW(hdc, delayText.c_str(), -1, &delayCopy,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
  }

  const bool custom = g_setupPreset == DelayPreset::Custom;
  SetTextColor(hdc, custom ? RGB(186, 199, 211) : RGB(94, 106, 116));
  DrawTextAt(hdc, 24, 314, T("setup.duration"));
  DrawTextAt(hdc, 306, 314, T("setup.interval"));
  DrawTextAt(hdc, 258, 314, T("unit.ms"));
  DrawTextAt(hdc, 540, 314, T("unit.ms"));
  DrawEditFrame(hdc, RECT{142, 306, 248, 332}, custom);
  DrawEditFrame(hdc, RECT{424, 306, 530, 332}, custom);
  if (!g_setupError.empty()) {
    SetTextColor(hdc, RGB(245, 125, 125));
    TextOutW(hdc, 24, 270, g_setupError.c_str(), static_cast<int>(g_setupError.size()));
  }
  DrawSegment(hdc, SetupCancelRect(), T("action.cancel"), false, accentBrush, normalBrush, accentPen);
  DrawSegment(hdc, SetupSaveRect(), T("action.start"), true, accentBrush, normalBrush, accentPen);
  SelectObject(hdc, oldFont);
  DeleteObject(labelFont);
  DeleteObject(bodyFont);
  DeleteObject(detailFont);
  DeleteObject(accentBrush);
  DeleteObject(normalBrush);
  DeleteObject(accentPen);
}

LRESULT CALLBACK SetupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, 600, 380, 14, 14), TRUE);
      if (g_dialogIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_dialogIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_dialogIcon));
      }
      AddDarkEdit(hwnd, std::to_wstring(TapHoldMs()), 142, 306, 106, 26, kSetupHold);
      AddDarkEdit(hwnd, std::to_wstring(TapGapMs()), 424, 306, 106, 26, kSetupGap);
      SetSetupPreset(hwnd, g_setupPreset);
      return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      HDC editDc = reinterpret_cast<HDC>(wp);
      HWND control = reinterpret_cast<HWND>(lp);
      SetTextColor(editDc, IsWindowEnabled(control) ? RGB(225, 234, 242) : RGB(105, 118, 128));
      SetBkColor(editDc, RGB(28, 35, 42));
      return reinterpret_cast<LRESULT>(g_dialogEditBrush);
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (Contains(SetupCloseRect(), pt) || Contains(SetupCancelRect(), pt)) {
        DestroyWindow(hwnd);
      } else if (Contains(SetupChineseRect(), pt) || Contains(SetupEnglishRect(), pt)) {
        gta5::app::l10n::SetLanguage(Contains(SetupEnglishRect(), pt)
                                         ? Language::English
                                         : Language::Chinese);
        g_setupError.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (Contains(SetupResidentRect(), pt) || Contains(SetupSilentRect(), pt)) {
        g_setupMode = Contains(SetupSilentRect(), pt) ? LaunchMode::Silent : LaunchMode::Resident;
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (Contains(SetupFastRect(), pt)) {
        SetSetupPreset(hwnd, DelayPreset::Fast);
      } else if (Contains(SetupSlowRect(), pt)) {
        SetSetupPreset(hwnd, DelayPreset::Slow);
      } else if (Contains(SetupCustomRect(), pt)) {
        SetSetupPreset(hwnd, DelayPreset::Custom);
        SetFocus(GetDlgItem(hwnd, kSetupHold));
      } else if (Contains(SetupSaveRect(), pt)) {
        int hold = 20;
        int gap = 20;
        if (g_setupPreset == DelayPreset::Slow) {
          hold = gap = 40;
        } else if (g_setupPreset == DelayPreset::Custom &&
                   !ReadDelayEdits(hwnd, kSetupHold, kSetupGap, hold, gap, g_setupError)) {
          return 0;
        }
        g_launchMode.store(static_cast<int>(g_setupMode), std::memory_order_relaxed);
        g_delayPreset.store(static_cast<int>(g_setupPreset), std::memory_order_relaxed);
        g_tapHoldMs.store(hold, std::memory_order_relaxed);
        g_tapGapMs.store(gap, std::memory_order_relaxed);
        g_setupComplete = true;
        SaveSettings();
        g_setupAccepted = true;
        DestroyWindow(hwnd);
      } else if (pt.y < 68) {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      }
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      DrawSetup(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      DestroyEditFont(GetDlgItem(hwnd, kSetupHold));
      DestroyEditFont(GetDlgItem(hwnd, kSetupGap));
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

RECT NoticeCloseRect() { return RECT{338, 18, 366, 46}; }
RECT NoticeOkRect() { return RECT{266, 130, 366, 160}; }

LRESULT CALLBACK NoticeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, 380, 172, 14, 14), TRUE);
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (Contains(NoticeCloseRect(), pt) || Contains(NoticeOkRect(), pt)) {
        DestroyWindow(hwnd);
      } else if (pt.y < 68) {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      }
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      DrawDialogBase(hdc, client, g_noticeTitle, T("notice.subtitle"), NoticeCloseRect());
      HFONT font = CreateUiFont(22, FW_SEMIBOLD);
      HGDIOBJ oldFont = SelectObject(hdc, font);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(186, 199, 211));
      RECT message{22, 82, 358, 120};
      DrawTextW(hdc, g_noticeMessage.c_str(), static_cast<int>(g_noticeMessage.size()), &message,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
      HBRUSH accentBrush = CreateSolidBrush(RGB(79, 220, 145));
      HBRUSH normalBrush = CreateSolidBrush(RGB(35, 43, 51));
      HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(67, 205, 132));
      DrawSegment(hdc, NoticeOkRect(), T("action.ok"), true, accentBrush, normalBrush, accentPen);
      SelectObject(hdc, oldFont);
      DeleteObject(font);
      DeleteObject(accentBrush);
      DeleteObject(normalBrush);
      DeleteObject(accentPen);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RunModalWindow(HWND hwnd) {
  if (!hwnd) return false;
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  MSG msg{};
  while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return true;
}

void CenterWindow(HWND hwnd, int width, int height, HWND owner = nullptr) {
  RECT area{};
  if (owner && GetWindowRect(owner, &area)) {
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  }
  int x = area.left + ((area.right - area.left) - width) / 2;
  int y = area.top + ((area.bottom - area.top) - height) / 2;
  SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

void SetHostWindow(HWND hwnd) { g_hostWnd = hwnd; }
void SetHudWindow(HWND hwnd) { g_hudWnd = hwnd; }
HWND HudWindow() { return g_hudWnd; }
void CollapseHud() {
  if (!g_hudWnd || !g_panelExpanded.load(std::memory_order_relaxed)) return;
  HideHoverHint();
  Collapse(g_hudWnd);
  UpdateWindow(g_hudWnd);
}
void LoadPersistentSettings() { LoadSettings(); }
bool NeedsFirstLaunchSetup() { return !g_setupComplete; }

bool RunFirstLaunchSetup(HINSTANCE instance, HICON icon) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = SetupProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"AutoHackFirstLaunchV1";
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered) return false;
  g_dialogIcon = icon;
  g_setupAccepted = false;
  g_setupMode = CurrentLaunchMode();
  g_setupPreset = static_cast<DelayPreset>(g_delayPreset.load(std::memory_order_relaxed));
  g_setupError.clear();
  if (!g_dialogEditBrush) g_dialogEditBrush = CreateSolidBrush(RGB(28, 35, 42));
  DPI_AWARENESS_CONTEXT previousDpi =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
  HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"AutoHackFirstLaunchV1", T("setup.title").c_str(),
                              WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 600, 380,
                              nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    return false;
  }
  CenterWindow(hwnd, 600, 380);
  if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
  RunModalWindow(hwnd);
  return g_setupAccepted;
}

void ShowNotice(HINSTANCE instance, HICON icon, const std::wstring& title, const std::wstring& message) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = NoticeProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"AutoHackNoticeV1";
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered) return;
  g_dialogIcon = icon;
  g_noticeTitle = title;
  g_noticeMessage = message;
  DPI_AWARENESS_CONTEXT previousDpi =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"AutoHackNoticeV1", title.c_str(), WS_POPUP,
                              CW_USEDEFAULT, CW_USEDEFAULT, 380, 172,
                              nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    return;
  }
  CenterWindow(hwnd, 380, 172);
  if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
  RunModalWindow(hwnd);
}

void ApplyHotkey(HWND hwnd) {
  if (!hwnd) return;
  UnregisterHotKey(hwnd, kHotkeyToggleId);
  RegisterHotKey(hwnd, kHotkeyToggleId, MOD_NOREPEAT,
                 static_cast<UINT>(g_hotkeyVk.load(std::memory_order_relaxed)));
}

int HotkeyId() { return kHotkeyToggleId; }
bool IsListeningHotkey() { return g_listeningHotkey.load(std::memory_order_relaxed); }
std::wstring HotkeyName() { return KeyName(g_hotkeyVk.load(std::memory_order_relaxed)); }
bool OverlayEnabled() { return !SilentMode() && g_overlayEnabled.load(std::memory_order_relaxed); }
void SetOverlayEnabled(bool enabled) {
  g_overlayEnabled.store(enabled, std::memory_order_relaxed);
  SaveSettings();
  Repaint();
}
LaunchMode CurrentLaunchMode() {
  return static_cast<LaunchMode>(g_launchMode.load(std::memory_order_relaxed));
}
bool SilentMode() { return CurrentLaunchMode() == LaunchMode::Silent; }
UINT ModeChangedMessage() { return kModeChangedMessage; }
int TapHoldMs() { return g_tapHoldMs.load(std::memory_order_relaxed); }
int TapGapMs() { return g_tapGapMs.load(std::memory_order_relaxed); }

void SetRunning(bool running) {
  g_running.store(running, std::memory_order_relaxed);
  Repaint();
}

void SetStatusText(const std::wstring& text) {
  std::lock_guard<std::mutex> lock(g_textMutex);
  g_status = text;
}

void SetLogText(const std::wstring& text) {
  std::lock_guard<std::mutex> lock(g_textMutex);
  g_lastLog = text;
}

void Repaint() {
  if (!g_hudWnd || g_repaintPending.exchange(true, std::memory_order_relaxed)) return;
  InvalidateRect(g_hudWnd, nullptr, FALSE);
}

RECT InitialHudRect() {
  RECT anchor{};
  HMONITOR monitor = nullptr;
  if (FindWindowRectByExe(L"Rockstar Games Launcher.exe", anchor) ||
      FindWindowRectByExe(L"steam.exe", anchor) ||
      FindWindowRectByExe(L"EADesktop.exe", anchor)) {
    monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  } else {
    POINT cursor{};
    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  }
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info)) info.rcMonitor = VirtualDesktopRect();
  const int left = info.rcMonitor.right - CurrentWidth();
  const int maxTop = std::max(static_cast<int>(info.rcMonitor.top),
                              static_cast<int>(info.rcMonitor.bottom) - CurrentHeight());
  const int top = std::clamp(static_cast<int>(info.rcMonitor.top) + 50,
                             static_cast<int>(info.rcMonitor.top),
                             maxTop);
  return RECT{left, top, info.rcMonitor.right, top + CurrentHeight()};
}

int HudWidth() { return CurrentWidth(); }
int HudHeight() { return CurrentHeight(); }

void ShowToggleNotification(bool enabled) {
  if (!SilentMode()) return;
  if (g_toastWnd && IsWindow(g_toastWnd)) DestroyWindow(g_toastWnd);
  g_toastEnabled = enabled;
  HWND foreground = GetForegroundWindow();
  HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  constexpr int width = 176;
  constexpr int height = 46;
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
  const int y = info.rcWork.bottom - height - 18;
  g_toastWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               L"Gta3In1SilentToastV1", T("notice.subtitle").c_str(), WS_POPUP,
                               x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!g_toastWnd) return;
  SetWindowPos(g_toastWnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(g_toastWnd, 1, 2000, nullptr);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (g_toastWnd == hwnd) g_toastWnd = nullptr;
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH background = CreateSolidBrush(RGB(18, 22, 27));
      HBRUSH indicator = CreateSolidBrush(g_toastEnabled ? RGB(75, 220, 140) : RGB(110, 120, 130));
      FillRect(hdc, &rc, background);
      SelectObject(hdc, indicator);
      SelectObject(hdc, GetStockObject(NULL_PEN));
      Ellipse(hdc, 18, 17, 30, 29);
      HFONT font = CreateUiFont(20, FW_SEMIBOLD);
      HGDIOBJ oldFont = SelectObject(hdc, font);
      SetBkMode(hdc, TRANSPARENT);
      RECT text{42, 0, rc.right - 10, rc.bottom};
      DrawCenteredText(hdc, text, T(g_toastEnabled ? "toast.on" : "toast.off"),
                       g_toastEnabled ? RGB(140, 244, 180) : RGB(190, 198, 205));
      SelectObject(hdc, oldFont);
      DeleteObject(font);
      DeleteObject(background);
      DeleteObject(indicator);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      if (g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_CREATE: {
      if (!g_dialogEditBrush) g_dialogEditBrush = CreateSolidBrush(RGB(28, 35, 42));
      const RECT hold = HoldEditRect();
      const RECT gap = GapEditRect();
      AddDarkEdit(hwnd, std::to_wstring(TapHoldMs()), hold.left, hold.top,
                  hold.right - hold.left, hold.bottom - hold.top, kDelayHold);
      AddDarkEdit(hwnd, std::to_wstring(TapGapMs()), gap.left, gap.top,
                  gap.right - gap.left, gap.bottom - gap.top, kDelayGap);
      SyncHudDelayControls(hwnd);
      SetTimer(hwnd, 1, 100, nullptr);
      return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
      HDC editDc = reinterpret_cast<HDC>(wp);
      HWND control = reinterpret_cast<HWND>(lp);
      SetTextColor(editDc, IsWindowEnabled(control) ? RGB(225, 234, 242) : RGB(105, 118, 128));
      SetBkColor(editDc, RGB(28, 35, 42));
      return reinterpret_cast<LRESULT>(g_dialogEditBrush);
    }
    case WM_COMMAND:
      if ((LOWORD(wp) == kDelayHold || LOWORD(wp) == kDelayGap) && HIWORD(wp) == EN_CHANGE) {
        SaveHudCustomDelay(hwnd);
        Touch();
      }
      return 0;
    case WM_TIMER:
      if (wp == 1) {
        if (g_listeningHotkey.load(std::memory_order_relaxed)) {
          for (int vk = 8; vk <= 254; ++vk) {
            if (!IsValidHotkeyVk(vk)) continue;
            if (GetAsyncKeyState(vk) & 1) {
              const int previous = g_pendingHotkeyVk.exchange(vk, std::memory_order_relaxed);
              if (previous != vk) Repaint();
              break;
            }
          }
        }
        if (g_panelExpanded.load(std::memory_order_relaxed) && !g_dragging &&
            !g_listeningHotkey.load(std::memory_order_relaxed) && g_lastInteractionTick != 0 &&
            GetTickCount64() - g_lastInteractionTick >= kHudAutoCollapseMs) {
          RECT wr{};
          POINT cursor{};
          if (GetWindowRect(hwnd, &wr) && GetCursorPos(&cursor) && !PtInRect(&wr, cursor)) Collapse(hwnd);
        }
      }
      return 0;
    case WM_NCHITTEST: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      RECT panel = PanelRect();
      return PtInRect(&panel, pt) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT panel = PanelRect();
      if (!g_panelExpanded.load(std::memory_order_relaxed)) {
        Expand(hwnd);
        return 0;
      }
      Touch();
      if (Contains(CollapseRect(panel), pt)) {
        Collapse(hwnd);
        return 0;
      }
      if (Contains(ResidentRect(), pt) || Contains(SilentRect(), pt)) {
        const LaunchMode next = Contains(SilentRect(), pt) ? LaunchMode::Silent : LaunchMode::Resident;
        if (next != CurrentLaunchMode()) {
          g_launchMode.store(static_cast<int>(next), std::memory_order_relaxed);
          SaveSettings();
          Repaint();
          if (g_hostWnd) PostMessageW(g_hostWnd, kModeChangedMessage, 0, 0);
        }
        return 0;
      }
      if (Contains(HotkeyActionRect(), pt)) {
        if (!g_listeningHotkey.load(std::memory_order_relaxed)) {
          g_pendingHotkeyVk.store(g_hotkeyVk.load(std::memory_order_relaxed), std::memory_order_relaxed);
          g_listeningHotkey.store(true, std::memory_order_relaxed);
        } else {
          const int pending = g_pendingHotkeyVk.load(std::memory_order_relaxed);
          if (IsValidHotkeyVk(pending)) {
            g_hotkeyVk.store(pending, std::memory_order_relaxed);
            ApplyHotkey(g_hostWnd);
            SaveSettings();
          }
          g_listeningHotkey.store(false, std::memory_order_relaxed);
        }
        Repaint();
        return 0;
      }
      if (Contains(AdminActionRect(), pt)) {
        RestartAsAdministrator(hwnd);
        return 0;
      }
      if (Contains(FastRect(), pt) || Contains(SlowRect(), pt)) {
        ApplyDelayPreset(Contains(FastRect(), pt) ? DelayPreset::Fast : DelayPreset::Slow);
        SyncHudDelayControls(hwnd);
        SaveSettings();
        Repaint();
        return 0;
      }
      if (Contains(CustomRect(), pt)) {
        g_delayPreset.store(static_cast<int>(DelayPreset::Custom), std::memory_order_relaxed);
        SyncHudDelayControls(hwnd);
        SaveSettings();
        SetFocus(GetDlgItem(hwnd, kDelayHold));
        Touch();
        return 0;
      }
      if (Contains(OverlayRect(), pt) && !SilentMode()) {
        SetOverlayEnabled(!g_overlayEnabled.load(std::memory_order_relaxed));
        return 0;
      }
      if (Contains(ChineseRect(), pt) || Contains(EnglishRect(), pt)) {
        gta5::app::l10n::SetLanguage(Contains(EnglishRect(), pt)
                                         ? Language::English
                                         : Language::Chinese);
        {
          std::lock_guard<std::mutex> lock(g_textMutex);
          g_status = T(g_running.load(std::memory_order_relaxed) ? "status.running" : "status.idle");
        }
        HideHoverHint();
        SaveSettings();
        Repaint();
        return 0;
      }
      if (Contains(HeaderRect(panel), pt) && !Contains(ExitRect(panel), pt)) {
        g_dragging = true;
        g_dragOffset = POINT{pt.x, pt.y};
        SetCapture(hwnd);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g_panelExpanded.load(std::memory_order_relaxed)) {
        Touch();
        if (!g_trackingHudMouse) {
          TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&tracking);
          g_trackingHudMouse = true;
        }
        POINT hoverPoint{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int target = 0;
        RECT anchor{};
        std::wstring hint;
        if (GetHoverHint(hoverPoint, target, anchor, hint)) {
          ShowHoverHint(hwnd, target, anchor, hint);
        } else {
          HideHoverHint();
        }
      }
      if (g_dragging) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &pt);
        RECT desired{pt.x - g_dragOffset.x, pt.y - g_dragOffset.y,
                     pt.x - g_dragOffset.x + CurrentWidth(), pt.y - g_dragOffset.y + CurrentHeight()};
        RECT clamped = ClampRect(desired);
        g_userPlaced = true;
        g_hudPos = POINT{clamped.left, clamped.top};
        HWND placeAfter = SilentMode() ? HWND_NOTOPMOST : HWND_TOPMOST;
        SetWindowPos(hwnd, placeAfter, clamped.left, clamped.top, CurrentWidth(), CurrentHeight(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      }
      return 0;
    case WM_MOUSELEAVE:
      g_trackingHudMouse = false;
      HideHoverHint();
      return 0;
    case WM_LBUTTONUP:
      if (g_dragging) {
        g_dragging = false;
        ReleaseCapture();
        return 0;
      }
      if (g_panelExpanded.load(std::memory_order_relaxed)) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (Contains(ExitRect(PanelRect()), pt) && g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      if (g_hintWnd) DestroyWindow(g_hintWnd);
      DestroyEditFont(GetDlgItem(hwnd, kDelayHold));
      DestroyEditFont(GetDlgItem(hwnd, kDelayGap));
      return 0;
    case WM_PAINT: {
      g_repaintPending.store(false, std::memory_order_relaxed);
      PAINTSTRUCT ps{};
      HDC paintDc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HDC memoryDc = CreateCompatibleDC(paintDc);
      HBITMAP buffer = CreateCompatibleBitmap(paintDc, rc.right, rc.bottom);
      HGDIOBJ oldBitmap = SelectObject(memoryDc, buffer);
      HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(memoryDc, &rc, clear);
      DeleteObject(clear);
      DrawPanel(memoryDc, PanelRect());
      BitBlt(paintDc, 0, 0, rc.right, rc.bottom, memoryDc, 0, 0, SRCCOPY);
      SelectObject(memoryDc, oldBitmap);
      DeleteObject(buffer);
      DeleteDC(memoryDc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace gta5::app::ui
