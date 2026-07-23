#pragma once

#include <windows.h>

#include <string>

namespace gta5::app::ui {

void SetHostWindow(HWND hwnd);
void SetHudWindow(HWND hwnd);
HWND HudWindow();

void LoadPersistentSettings();
void ApplyHotkey(HWND hwnd);
int HotkeyId();
bool IsListeningHotkey();
std::wstring HotkeyName();

bool OverlayEnabled();
void SetOverlayEnabled(bool enabled);
int TapHoldMs();
int TapGapMs();

void SetRunning(bool running);
void SetStatusText(const std::wstring& text);
void SetLogText(const std::wstring& text);
void Repaint();

RECT InitialHudRect();
int HudWidth();
int HudHeight();
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace gta5::app::ui
