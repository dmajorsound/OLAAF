// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The olAAF import dialog: track list with colors, timeline preview and import options.

#pragma once
#include "../core/importer.h"

#include <functional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif
#undef min
#undef max

namespace olaaf {

// REAPER services the dialog needs (provided by main.cpp, which owns the REAPER API)
struct DialogHost {
  std::function<bool(const char *title, const char *caption, std::string *value)> askText;
  std::function<bool(HWND parent, uint32_t *rgb)> pickColor; // 0xRRGGBB
  std::function<bool(const char *title, std::string *path)> pickFile;
  std::function<double()> editCursor;
};

// Shows the dialog. Returns true when the user pressed Import; `opt` and the session's
// track flags / colors then hold the choices.
bool showImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host);
// The same window without blocking REAPER: returns at once; onClose runs after the window is closed
// (accepted = Build session). Only one can be open; a second call brings it to the front and returns nullptr.
HWND openImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host,
                      std::function<void(bool accepted)> onClose);

void dialogSetInstance(void *hInstance);
// for REAPER's accelerator hook ("accelerator" plugin_register): keys while the import dialog is open
int dialogTranslateKey(MSG *msg);

} // namespace olaaf
