// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Rounded shapes, smooth on every system (GDI+ on Windows, Core Graphics through SWELL on macOS).
// Coordinates are device pixels.

#pragma once
#include "theme.h"

namespace olaaf {
// a filled rounded rectangle
void olRoundFill(HDC dc, double l, double t, double r, double b, double rad, COLORREF c);
// paints the corners outside a rounded rectangle in bg and draws its 1 px border in line
// (for a panel whose content was drawn square)
void olRoundCorners(HDC dc, double l, double t, double r, double b, double rad, COLORREF bg, COLORREF line);
} // namespace olaaf
