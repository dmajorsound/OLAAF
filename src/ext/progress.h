// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Runs the import in a background thread and, when it takes longer than a moment,
// shows the olAAF progress window: bouncing meter bars, a progress bar and a Cancel button.

#pragma once
#include "../core/importer.h"

#include <functional>

#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif
#undef min
#undef max

namespace olaaf {

// `work` runs on a worker thread. It must only touch olAAF code, never the REAPER API.
// Returns what `work` returned; *cancelled is set when the user pressed Cancel.
bool runWithProgress(HWND parent, const std::function<bool(const Progress &)> &work, bool *cancelled);

void progressSetInstance(void *hInstance);

} // namespace olaaf
