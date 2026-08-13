#pragma once

#include "game_window.h"

namespace gta5::capture::dxgi {

CaptureStatus CaptureLatest(GameFrame& frame, const RECT& client,
                            const RECT* screenRegion);
void Stop();

}  // namespace gta5::capture::dxgi
