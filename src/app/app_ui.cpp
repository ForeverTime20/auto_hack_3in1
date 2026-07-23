#include "app_ui.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>

namespace gta5::app::ui {
namespace {

constexpr int kHotkeyToggleId = 2001;
constexpr int kHudWidth = 300;
constexpr int kHudMiniWidth = 236;
constexpr int kHudMiniHeight = 42;
constexpr int kHudCollapsedHeight = 118;
constexpr int kHudExpandedHeight = 260;
constexpr int kHudMargin = 18;
constexpr ULONGLONG kHudAutoCollapseMs = 3500;

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

HWND g_hostWnd = nullptr;
HWND g_hudWnd = nullptr;
std::atomic<bool> g_repaintPending{false};
std::atomic<bool> g_running{false};
std::atomic<int> g_hotkeyVk{VK_F6};
std::atomic<int> g_pendingHotkeyVk{VK_F6};
std::atomic<int> g_tapHoldMs{20};
std::atomic<int> g_tapGapMs{30};
std::atomic<bool> g_listeningHotkey{false};
std::atomic<bool> g_panelExpanded{false};
std::atomic<bool> g_settingsExpanded{false};
std::atomic<bool> g_overlayEnabled{true};
std::mutex g_textMutex;
std::wstring g_status = L"idle";
std::wstring g_lastLog;
bool g_userPlaced = false;
POINT g_hudPos{0, 0};
bool g_dragging = false;
POINT g_dragOffset{0, 0};
ULONGLONG g_lastInteractionTick = 0;

int CurrentHeight() {
  if (!g_panelExpanded.load(std::memory_order_relaxed)) return kHudMiniHeight;
  return g_settingsExpanded.load(std::memory_order_relaxed) ? kHudExpandedHeight : kHudCollapsedHeight;
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

void SaveSettings() {
  std::ofstream out(ExeSiblingPath(L"setting.ini").c_str(), std::ios::trunc);
  if (!out) return;
  out << "hotkey_vk=" << g_hotkeyVk.load(std::memory_order_relaxed) << "\n";
  out << "overlay_cursor=" << (g_overlayEnabled.load(std::memory_order_relaxed) ? 1 : 0) << "\n";
  out << "tap_hold_ms=" << g_tapHoldMs.load(std::memory_order_relaxed) << "\n";
  out << "tap_gap_ms=" << g_tapGapMs.load(std::memory_order_relaxed) << "\n";
}

void LoadSettings() {
  int hotkey = VK_F6;
  int overlay = 1;
  int tapHold = 20;
  int tapGap = 30;
  bool ok = false;
  bool needsSave = false;
  std::ifstream in(ExeSiblingPath(L"setting.ini").c_str());
  if (in) {
    std::string line;
    bool sawHotkey = false;
    bool sawOverlay = false;
    bool sawTapHold = false;
    bool sawTapGap = false;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      try {
        const std::string key = line.substr(0, eq);
        const int value = std::stoi(line.substr(eq + 1));
        if (key == "hotkey_vk") { hotkey = value; sawHotkey = true; }
        else if (key == "overlay_cursor") { overlay = value; sawOverlay = true; }
        else if (key == "tap_hold_ms") { tapHold = value; sawTapHold = true; }
        else if (key == "tap_gap_ms") { tapGap = value; sawTapGap = true; }
      } catch (...) {
        ok = false;
        break;
      }
    }
    ok = sawHotkey && sawOverlay && IsValidHotkeyVk(hotkey) && (overlay == 0 || overlay == 1);
    if (!sawTapHold || tapHold < 1 || tapHold > 250) { tapHold = 20; needsSave = true; }
    if (!sawTapGap || tapGap < 1 || tapGap > 250) { tapGap = 30; needsSave = true; }
  }
  if (!ok) {
    hotkey = VK_F6;
    overlay = 1;
    tapHold = 20;
    tapGap = 30;
    needsSave = true;
  }
  g_hotkeyVk.store(hotkey, std::memory_order_relaxed);
  g_pendingHotkeyVk.store(hotkey, std::memory_order_relaxed);
  g_overlayEnabled.store(overlay != 0, std::memory_order_relaxed);
  g_tapHoldMs.store(ClampTapMs(tapHold), std::memory_order_relaxed);
  g_tapGapMs.store(ClampTapMs(tapGap), std::memory_order_relaxed);
  if (needsSave) SaveSettings();
}

RECT VirtualDesktopRect() {
  return RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
              GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
              GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

RECT ClampRect(RECT panel) {
  RECT desktop = VirtualDesktopRect();
  const int width = panel.right - panel.left;
  const int height = panel.bottom - panel.top;
  const int minLeft = desktop.left + kHudMargin;
  const int minTop = desktop.top + kHudMargin;
  const int maxLeft = std::max(minLeft, static_cast<int>(desktop.right) - width - kHudMargin);
  const int maxTop = std::max(minTop, static_cast<int>(desktop.bottom) - height - kHudMargin);
  panel.left = std::clamp(static_cast<int>(panel.left), minLeft, maxLeft);
  panel.top = std::clamp(static_cast<int>(panel.top), minTop, maxTop);
  panel.right = panel.left + width;
  panel.bottom = panel.top + height;
  return panel;
}

RECT PanelRect() { return RECT{0, 0, CurrentWidth(), CurrentHeight()}; }
RECT ExitRect(const RECT& p) { return RECT{p.right - 36, p.top + 10, p.right - 14, p.top + 32}; }
RECT CollapseRect(const RECT& p) { return RECT{p.right - 66, p.top + 10, p.right - 44, p.top + 32}; }
RECT HeaderRect(const RECT& p) { return RECT{p.left, p.top, p.right, p.top + 42}; }
RECT SettingsRect(const RECT& p) { return RECT{p.right - 42, p.top + 84, p.right - 16, p.top + 104}; }
RECT HotkeyRect(const RECT& p) { return RECT{p.left + 92, p.top + 112, p.left + 190, p.top + 140}; }
RECT ConfirmRect(const RECT& p) { return RECT{p.left + 204, p.top + 112, p.left + 286, p.top + 140}; }
RECT OverlayRect(const RECT& p) { return RECT{p.left + 18, p.top + 150, p.left + 36, p.top + 168}; }
RECT HoldMinusRect(const RECT& p) { return RECT{p.left + 162, p.top + 190, p.left + 184, p.top + 214}; }
RECT HoldValueRect(const RECT& p) { return RECT{p.left + 188, p.top + 190, p.left + 226, p.top + 214}; }
RECT HoldPlusRect(const RECT& p) { return RECT{p.left + 230, p.top + 190, p.left + 252, p.top + 214}; }
RECT GapMinusRect(const RECT& p) { return RECT{p.left + 162, p.top + 220, p.left + 184, p.top + 244}; }
RECT GapValueRect(const RECT& p) { return RECT{p.left + 188, p.top + 220, p.left + 226, p.top + 244}; }
RECT GapPlusRect(const RECT& p) { return RECT{p.left + 230, p.top + 220, p.left + 252, p.top + 244}; }
bool Contains(RECT rect, POINT point) { return PtInRect(&rect, point) != FALSE; }

void ResizeHud(HWND hwnd) {
  if (!hwnd) return;
  RECT wr{};
  GetWindowRect(hwnd, &wr);
  RECT desired{wr.left, wr.top, wr.left + CurrentWidth(), wr.top + CurrentHeight()};
  RECT clamped = ClampRect(desired);
  g_hudPos = POINT{clamped.left, clamped.top};
  g_userPlaced = true;
  SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, CurrentWidth(), CurrentHeight(),
               SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Touch() { g_lastInteractionTick = GetTickCount64(); }

void Collapse(HWND hwnd) {
  g_panelExpanded.store(false, std::memory_order_relaxed);
  g_settingsExpanded.store(false, std::memory_order_relaxed);
  g_listeningHotkey.store(false, std::memory_order_relaxed);
  ResizeHud(hwnd);
  Repaint();
}

void Expand(HWND hwnd) {
  g_panelExpanded.store(true, std::memory_order_relaxed);
  g_listeningHotkey.store(false, std::memory_order_relaxed);
  Touch();
  ResizeHud(hwnd);
  Repaint();
}

void DrawPanel(HDC hdc, const RECT& panel) {
  const int px = panel.left;
  const int py = panel.top;
  const bool running = g_running.load(std::memory_order_relaxed);
  std::wstring status;
  {
    std::lock_guard<std::mutex> lock(g_textMutex);
    status = g_status;
  }

  HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(70, 255, 120));
  HPEN closePen = CreatePen(PS_SOLID, 2, RGB(255, 120, 120));
  HBRUSH panelBrush = CreateSolidBrush(RGB(10, 14, 20));
  HBRUSH headerBrush = CreateSolidBrush(RGB(18, 28, 42));
  HBRUSH runningBrush = CreateSolidBrush(RGB(255, 150, 45));
  HBRUSH readyBrush = CreateSolidBrush(RGB(80, 255, 140));
  HBRUSH dimBrush = CreateSolidBrush(RGB(30, 42, 54));
  HBRUSH closeBrush = CreateSolidBrush(RGB(42, 20, 26));
  HBRUSH actionBrush = CreateSolidBrush(RGB(22, 48, 44));
  HFONT font = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
  HGDIOBJ oldFont = SelectObject(hdc, font);
  HGDIOBJ oldPen = SelectObject(hdc, borderPen);
  HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
  SetBkMode(hdc, TRANSPARENT);

  SelectObject(hdc, panelBrush);
  RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom,
            g_panelExpanded.load(std::memory_order_relaxed) ? 14 : 18,
            g_panelExpanded.load(std::memory_order_relaxed) ? 14 : 18);
  if (!g_panelExpanded.load(std::memory_order_relaxed)) {
    RECT led{px + 14, py + 15, px + 26, py + 27};
    SelectObject(hdc, running ? runningBrush : readyBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, led.left, led.top, led.right, led.bottom);
    const wchar_t* title = running ? L"RUNNING" : L"READY";
    SetTextColor(hdc, running ? RGB(255, 170, 70) : RGB(145, 255, 175));
    TextOutW(hdc, px + 36, py + 7, title, static_cast<int>(wcslen(title)));
    SetTextColor(hdc, RGB(210, 220, 230));
    TextOutW(hdc, px + 104, py + 8, status.c_str(), static_cast<int>(std::min<size_t>(status.size(), 14)));
  } else {
    SelectObject(hdc, headerBrush);
    RoundRect(hdc, panel.left, panel.top, panel.right, panel.top + 42, 14, 14);
    SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    SelectObject(hdc, borderPen);
    RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom, 14, 14);
    RECT led{px + 14, py + 13, px + 26, py + 25};
    SelectObject(hdc, running ? runningBrush : readyBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, led.left, led.top, led.right, led.bottom);
    const wchar_t* title = running ? L"RUNNING" : L"READY";
    SetTextColor(hdc, running ? RGB(255, 170, 70) : RGB(145, 255, 175));
    TextOutW(hdc, px + 36, py + 7, title, static_cast<int>(wcslen(title)));
    const std::wstring hotkey = L"Hotkey " + KeyName(g_hotkeyVk.load(std::memory_order_relaxed));
    SetTextColor(hdc, RGB(210, 220, 230));
    TextOutW(hdc, px + 122, py + 8, hotkey.c_str(), static_cast<int>(std::min<size_t>(hotkey.size(), 12)));

    const RECT collapse = CollapseRect(panel);
    const RECT exit = ExitRect(panel);
    SelectObject(hdc, dimBrush);
    SelectObject(hdc, borderPen);
    RoundRect(hdc, collapse.left, collapse.top, collapse.right, collapse.bottom, 8, 8);
    MoveToEx(hdc, collapse.left + 6, collapse.top + 11, nullptr);
    LineTo(hdc, collapse.right - 6, collapse.top + 11);
    SelectObject(hdc, closeBrush);
    SelectObject(hdc, closePen);
    RoundRect(hdc, exit.left, exit.top, exit.right, exit.bottom, 8, 8);
    MoveToEx(hdc, exit.left + 7, exit.top + 7, nullptr);
    LineTo(hdc, exit.right - 7, exit.bottom - 7);
    MoveToEx(hdc, exit.right - 7, exit.top + 7, nullptr);
    LineTo(hdc, exit.left + 7, exit.bottom - 7);

    SetTextColor(hdc, RGB(145, 155, 170));
    TextOutW(hdc, px + 14, py + 54, status.c_str(), static_cast<int>(std::min<size_t>(status.size(), 48)));
    const bool settings = g_settingsExpanded.load(std::memory_order_relaxed);
    const RECT toggle = SettingsRect(panel);
    SetTextColor(hdc, RGB(120, 135, 150));
    TextOutW(hdc, px + 14, py + 84, L"SETTINGS", 8);
    POINT tri[3]{};
    if (settings) {
      tri[0] = POINT{toggle.left + 5, toggle.top + 7};
      tri[1] = POINT{toggle.right - 5, toggle.top + 7};
      tri[2] = POINT{(toggle.left + toggle.right) / 2, toggle.bottom - 5};
    } else {
      tri[0] = POINT{toggle.left + 7, toggle.top + 4};
      tri[1] = POINT{toggle.left + 7, toggle.bottom - 4};
      tri[2] = POINT{toggle.right - 5, (toggle.top + toggle.bottom) / 2};
    }
    SelectObject(hdc, readyBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);

    if (settings) {
      const RECT hotkeyBox = HotkeyRect(panel);
      const RECT confirm = ConfirmRect(panel);
      const RECT overlay = OverlayRect(panel);
      const bool listening = g_listeningHotkey.load(std::memory_order_relaxed);
      SetTextColor(hdc, RGB(180, 195, 210));
      TextOutW(hdc, px + 14, py + 118, L"Hotkey", 6);
      SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
      SelectObject(hdc, borderPen);
      RoundRect(hdc, hotkeyBox.left, hotkeyBox.top, hotkeyBox.right, hotkeyBox.bottom, 7, 7);
      const std::wstring key = KeyName(listening ? g_pendingHotkeyVk.load(std::memory_order_relaxed)
                                                 : g_hotkeyVk.load(std::memory_order_relaxed));
      SetTextColor(hdc, listening ? RGB(255, 210, 90) : RGB(225, 235, 245));
      TextOutW(hdc, hotkeyBox.left + 10, hotkeyBox.top + 6, key.c_str(), static_cast<int>(std::min<size_t>(key.size(), 10)));
      SelectObject(hdc, actionBrush);
      SelectObject(hdc, borderPen);
      RoundRect(hdc, confirm.left, confirm.top, confirm.right, confirm.bottom, 7, 7);
      const wchar_t* action = listening ? L"OK" : L"Change";
      SetTextColor(hdc, RGB(145, 255, 175));
      TextOutW(hdc, confirm.left + (listening ? 24 : 12), confirm.top + 6, action, static_cast<int>(wcslen(action)));

      const bool overlayOn = g_overlayEnabled.load(std::memory_order_relaxed);
      SelectObject(hdc, overlayOn ? readyBrush : dimBrush);
      SelectObject(hdc, borderPen);
      Rectangle(hdc, overlay.left, overlay.top, overlay.right, overlay.bottom);
      if (overlayOn) {
        MoveToEx(hdc, overlay.left + 4, overlay.top + 9, nullptr);
        LineTo(hdc, overlay.left + 8, overlay.bottom - 4);
        LineTo(hdc, overlay.right - 4, overlay.top + 4);
      }
      SetTextColor(hdc, RGB(180, 195, 210));
      TextOutW(hdc, px + 46, py + 150, L"Overlay cursor", 14);
      SetTextColor(hdc, RGB(120, 135, 150));
      TextOutW(hdc, px + 14, py + 178, L"AUTO KEY", 8);
      SetTextColor(hdc, RGB(180, 195, 210));
      TextOutW(hdc, px + 14, py + 195, L"Press duration", 14);
      TextOutW(hdc, px + 14, py + 225, L"Press interval", 14);

      auto drawButton = [&](RECT r, const wchar_t* text) {
        SelectObject(hdc, actionBrush);
        SelectObject(hdc, borderPen);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
        SetTextColor(hdc, RGB(145, 255, 175));
        TextOutW(hdc, r.left + 7, r.top + 4, text, static_cast<int>(wcslen(text)));
      };
      auto drawValue = [&](RECT r, int value) {
        SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        SelectObject(hdc, borderPen);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
        const std::wstring text = std::to_wstring(value);
        SetTextColor(hdc, RGB(225, 235, 245));
        TextOutW(hdc, r.left + 8, r.top + 4, text.c_str(), static_cast<int>(text.size()));
      };
      drawButton(HoldMinusRect(panel), L"-");
      drawValue(HoldValueRect(panel), g_tapHoldMs.load(std::memory_order_relaxed));
      drawButton(HoldPlusRect(panel), L"+");
      drawButton(GapMinusRect(panel), L"-");
      drawValue(GapValueRect(panel), g_tapGapMs.load(std::memory_order_relaxed));
      drawButton(GapPlusRect(panel), L"+");
      SetTextColor(hdc, RGB(180, 195, 210));
      TextOutW(hdc, px + 260, py + 195, L"ms", 2);
      TextOutW(hdc, px + 260, py + 225, L"ms", 2);
    }
  }

  SelectObject(hdc, oldFont);
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(font);
  DeleteObject(borderPen);
  DeleteObject(closePen);
  DeleteObject(panelBrush);
  DeleteObject(headerBrush);
  DeleteObject(runningBrush);
  DeleteObject(readyBrush);
  DeleteObject(dimBrush);
  DeleteObject(closeBrush);
  DeleteObject(actionBrush);
}

}  // namespace

void SetHostWindow(HWND hwnd) { g_hostWnd = hwnd; }
void SetHudWindow(HWND hwnd) { g_hudWnd = hwnd; }
HWND HudWindow() { return g_hudWnd; }
void LoadPersistentSettings() { LoadSettings(); }
void ApplyHotkey(HWND hwnd) {
  if (!hwnd) return;
  UnregisterHotKey(hwnd, kHotkeyToggleId);
  RegisterHotKey(hwnd, kHotkeyToggleId, MOD_NOREPEAT, static_cast<UINT>(g_hotkeyVk.load(std::memory_order_relaxed)));
}
int HotkeyId() { return kHotkeyToggleId; }
bool IsListeningHotkey() { return g_listeningHotkey.load(std::memory_order_relaxed); }
std::wstring HotkeyName() { return KeyName(g_hotkeyVk.load(std::memory_order_relaxed)); }
bool OverlayEnabled() { return g_overlayEnabled.load(std::memory_order_relaxed); }
void SetOverlayEnabled(bool enabled) { g_overlayEnabled.store(enabled, std::memory_order_relaxed); SaveSettings(); Repaint(); }
int TapHoldMs() { return g_tapHoldMs.load(std::memory_order_relaxed); }
int TapGapMs() { return g_tapGapMs.load(std::memory_order_relaxed); }
void SetRunning(bool running) {
  g_running.store(running, std::memory_order_relaxed);
  if (running && g_hudWnd) Collapse(g_hudWnd);
  else Repaint();
}
void SetStatusText(const std::wstring& text) { std::lock_guard<std::mutex> lock(g_textMutex); g_status = text; }
void SetLogText(const std::wstring& text) { std::lock_guard<std::mutex> lock(g_textMutex); g_lastLog = text; }
void Repaint() {
  bool expected = false;
  if (g_hudWnd && g_repaintPending.compare_exchange_strong(expected, true)) {
    InvalidateRect(g_hudWnd, nullptr, FALSE);
  }
}

RECT InitialHudRect() {
  if (g_userPlaced) return ClampRect(RECT{g_hudPos.x, g_hudPos.y, g_hudPos.x + CurrentWidth(), g_hudPos.y + CurrentHeight()});
  const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  if (virtualWidth == 5760) {
    const int left = virtualX + 3840 - CurrentWidth() - kHudMargin - 28;
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN) + 50;
    return ClampRect(RECT{left, top, left + CurrentWidth(), top + CurrentHeight()});
  }

  HMONITOR monitor = nullptr;
  RECT gameRect{};
  if (FindWindowRectByExe(L"GTA5_Enhanced.exe", gameRect) || FindWindowRectByExe(L"GTA5.exe", gameRect)) {
    monitor = MonitorFromRect(&gameRect, MONITOR_DEFAULTTONEAREST);
  }
  if (!monitor) {
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_hostWnd && foreground != g_hudWnd) {
      RECT foregroundRect{};
      if (GetWindowRect(foreground, &foregroundRect)) {
        monitor = MonitorFromRect(&foregroundRect, MONITOR_DEFAULTTONEAREST);
      }
    }
  }
  if (!monitor) {
    POINT cursor{};
    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  }
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);
  const int left = info.rcMonitor.right - CurrentWidth() - kHudMargin - 28;
  const int top = info.rcMonitor.top + 50;
  return ClampRect(RECT{left, top, left + CurrentWidth(), top + CurrentHeight()});
}
int HudWidth() { return CurrentWidth(); }
int HudHeight() { return CurrentHeight(); }

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      if (g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_CREATE:
      SetTimer(hwnd, 1, 100, nullptr);
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
      RECT panel = PanelRect();
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      if (!g_panelExpanded.load(std::memory_order_relaxed)) return PtInRect(&panel, pt) ? HTCLIENT : HTTRANSPARENT;
      const bool settings = g_settingsExpanded.load(std::memory_order_relaxed);
      const bool hit = Contains(ExitRect(panel), pt) || Contains(CollapseRect(panel), pt) ||
          Contains(HeaderRect(panel), pt) || Contains(SettingsRect(panel), pt) ||
          (settings && (Contains(ConfirmRect(panel), pt) || Contains(OverlayRect(panel), pt) ||
                        Contains(HoldMinusRect(panel), pt) || Contains(HoldPlusRect(panel), pt) ||
                        Contains(GapMinusRect(panel), pt) || Contains(GapPlusRect(panel), pt)));
      return hit ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_LBUTTONDOWN: {
      RECT panel = PanelRect();
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (!g_panelExpanded.load(std::memory_order_relaxed)) { Expand(hwnd); return 0; }
      Touch();
      if (Contains(CollapseRect(panel), pt)) { Collapse(hwnd); return 0; }
      if (Contains(SettingsRect(panel), pt)) {
        g_settingsExpanded.store(!g_settingsExpanded.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_listeningHotkey.store(false, std::memory_order_relaxed);
        ResizeHud(hwnd);
        Repaint();
        return 0;
      }
      if (g_settingsExpanded.load(std::memory_order_relaxed) && Contains(ConfirmRect(panel), pt)) {
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
      if (g_settingsExpanded.load(std::memory_order_relaxed) && Contains(OverlayRect(panel), pt)) {
        g_overlayEnabled.store(!g_overlayEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        SaveSettings();
        Repaint();
        return 0;
      }
      if (g_settingsExpanded.load(std::memory_order_relaxed)) {
        bool changed = true;
        if (Contains(HoldMinusRect(panel), pt)) g_tapHoldMs.store(ClampTapMs(TapHoldMs() - 1), std::memory_order_relaxed);
        else if (Contains(HoldPlusRect(panel), pt)) g_tapHoldMs.store(ClampTapMs(TapHoldMs() + 1), std::memory_order_relaxed);
        else if (Contains(GapMinusRect(panel), pt)) g_tapGapMs.store(ClampTapMs(TapGapMs() - 1), std::memory_order_relaxed);
        else if (Contains(GapPlusRect(panel), pt)) g_tapGapMs.store(ClampTapMs(TapGapMs() + 1), std::memory_order_relaxed);
        else changed = false;
        if (changed) { SaveSettings(); Repaint(); return 0; }
      }
      if (Contains(HeaderRect(panel), pt) && !Contains(ExitRect(panel), pt)) {
        g_dragging = true;
        g_dragOffset = POINT{pt.x - panel.left, pt.y - panel.top};
        SetCapture(hwnd);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g_panelExpanded.load(std::memory_order_relaxed)) Touch();
      if (g_dragging) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &pt);
        RECT desired{pt.x - g_dragOffset.x, pt.y - g_dragOffset.y,
                     pt.x - g_dragOffset.x + CurrentWidth(), pt.y - g_dragOffset.y + CurrentHeight()};
        RECT clamped = ClampRect(desired);
        g_userPlaced = true;
        g_hudPos = POINT{clamped.left, clamped.top};
        SetWindowPos(hwnd, HWND_TOPMOST, clamped.left, clamped.top, CurrentWidth(), CurrentHeight(),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
      }
      return 0;
    case WM_LBUTTONUP:
      if (g_dragging) { g_dragging = false; ReleaseCapture(); return 0; }
      if (g_panelExpanded.load(std::memory_order_relaxed)) {
        RECT panel = PanelRect();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (Contains(ExitRect(panel), pt) && g_hostWnd) PostMessageW(g_hostWnd, WM_CLOSE, 0, 0);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
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
