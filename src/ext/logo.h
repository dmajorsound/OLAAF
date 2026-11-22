// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif

namespace olaaf {
// Footer signature: the dmajorsound mark at 'right', "dmajorsound.com" in small type to its left,
// vertically centered on 'midY'. markH is the mark height in device pixels. Returns the left edge.
int drawDmajorLogo(HDC dc, int right, int midY, int markH, COLORREF markColor, COLORREF textColor, HFONT font);
} // namespace olaaf
