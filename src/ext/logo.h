// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif

namespace olaaf {
// Draws the dmajorsound logo (mark plus spaced "DMAJORSOUND") as one line, right-aligned at
// 'right', vertically centered on 'midY'. markH is the mark height in device pixels.
// Returns the left edge of what was drawn.
int drawDmajorLogo(HDC dc, int right, int midY, int markH, COLORREF color, HFONT font);
} // namespace olaaf
