// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The UI fonts, embedded in the extension and registered for this process only, so OLAAF looks the same on
// Mac and Windows: Mulish for text, Red Hat Mono for timecodes and numbers (as MARK). Built from the
// originals by tools/make_fonts.py: text Regular 400 / Bold = SemiBold 600, mono Regular = Medium 500.

#pragma once

namespace olaaf {
// Face name to pass to CreateFont (registers the embedded fonts on first use;
// falls back to the system font if that fails).
const char *uiFontFace(bool mono);
} // namespace olaaf
