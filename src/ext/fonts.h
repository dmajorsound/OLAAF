// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The UI font: IBM Plex Sans and IBM Plex Mono, embedded in the extension and
// registered for this process only, so OLAAF looks the same on Mac and Windows.

#pragma once

namespace olaaf {
// Face name to pass to CreateFont (registers the embedded fonts on first use;
// falls back to the system font if that fails).
const char *uiFontFace(bool mono);
} // namespace olaaf
