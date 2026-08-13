#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <vector>

namespace gta5::capture {

enum class Backend {
  Gdi = 0,
  DxgiExperimental = 1,
};

enum class CaptureStatus {
  NewFrame,
  NoNewFrame,
  Error,
};

void SetBackend(Backend backend);
Backend CurrentBackend();
bool LastCaptureSucceeded();

bool FindGameWindow();
bool GetGameClientRect(RECT& rect);
void ClearGameWindow();

struct GameFrame {
  int screenX = 0;
  int screenY = 0;
  int screenW = 0;
  int screenH = 0;
  int width = 0;
  int height = 0;
  int clientHeight = 0;
  int clientWidth = 0;
  std::uint64_t windowGeneration = 0;
  LONGLONG captureQpc = 0;
  double toScreenX = 1.0;
  double toScreenY = 1.0;
  std::vector<std::uint32_t> bgra;
};

bool CaptureGameFrame(GameFrame& frame, const RECT* screenRegion = nullptr);
CaptureStatus CaptureGameFrameStatus(GameFrame& frame, const RECT* screenRegion = nullptr);
bool CaptureGameFrameFromClientRect(GameFrame& frame, const RECT& clientRect,
                                    const RECT* screenRegion = nullptr);

}  // namespace gta5::capture
