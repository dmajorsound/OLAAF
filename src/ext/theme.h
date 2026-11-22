// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Light and dark colors shared by the dialog and the progress window
// (same palette as the other dmajorsound plugins).

#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif
#undef min
#undef max

namespace olaaf {

struct Theme {
  COLORREF face, panel, line, soft, track, mute, ink, blue, blueSoft, blueLine, red, green, thumb, empty, off;
};

void setDarkTheme(bool dark);
bool darkTheme();
const Theme &theme();

} // namespace olaaf
