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
// one file to listen to: part of it, at a gain, to the left, the right or both outputs
struct PreviewPart {
  std::string path;
  double offset = 0, length = 0; // seconds in the file
  double gain = 1.0;
  int side = 0;                  // 0 both, 1 left only, 2 right only (mono file of an L / R pair), 3 stereo reversed
};

// one event for listening to the whole timeline
struct MixEvent {
  std::string path;
  double pos = 0, len = 0, offset = 0; // on the timeline, and where in the file
  double gain = 1.0, fadeIn = 0, fadeOut = 0;
  int side = 0;                        // as PreviewPart
};

struct DialogHost {
  std::function<bool(const char *title, const char *caption, std::string *value)> askText;
  std::function<bool(HWND parent, uint32_t *rgb)> pickColor; // 0xRRGGBB
  std::function<bool(const char *title, std::string *path)> pickFile;
  std::function<double()> editCursor;
  // tracks of the open REAPER project (GUID, name), for putting AAF tracks on existing ones; empty = none
  std::function<std::vector<std::pair<std::string, std::string>>()> projectTracks;
  // small settings kept between imports (window size)
  std::function<std::string(const char *key)> getSetting;
  std::function<void(const char *key, const std::string &value)> setSetting;
  // frame rate of the REAPER project (and whether it is drop frame); empty = unknown
  std::function<double(bool *dropFrame)> projectFps;
  // sample rate the REAPER project runs at (the project's own or the audio device's); 0 = unknown
  std::function<double()> projectSrate;
  // listening through REAPER's preview: start (false if REAPER can't read the file), stop,
  // and how far it got in seconds (-1 when it has stopped)
  std::function<bool(const std::vector<PreviewPart> &parts)> playPreview;
  std::function<void()> stopPreview;
  std::function<double()> previewPos;
  // listening to the timeline: every event mixed, from `start` (timeline seconds); previewPos then
  // returns the timeline position
  std::function<bool(const std::vector<MixEvent> &events, double start, double end)> playTimeline;
  // update check on GitHub: 0 still asking, 1 a newer release (tag, page), 2 nothing newer or no answer
  std::function<int(std::string *tag, std::string *url)> updateState;
  std::function<void(const std::string &url)> openUrl;
  // the choices made for one AAF (file name from choicesFileName), kept between imports
  std::function<std::string(const std::string &name)> loadChoices;
  std::function<void(const std::string &name, const std::string &text)> saveChoices;
  // reels: more AAFs in the same import. Reel 0 is the one the window opened with.
  std::function<int()> reelCount;
  std::function<bool(int i, Importer **imp, ImportOptions **opt)> reelAt;
  std::function<int(HWND parent, std::string *message)> addReels; // asks for AAFs; returns how many were added
  std::function<void(int i)> removeReel;                          // the first one is only left out
  std::function<bool(int i)> reelHidden;                          // a closed tab that stays loaded
  // right-click on an event: its file in the Finder / Explorer, its path on the clipboard
  std::function<void(const std::string &path)> revealFile;
  std::function<void(HWND hwnd, const std::string &text)> copyText;
  // where an AAF's embedded audio is unpacked for waveforms and listening (a folder of its own per AAF)
  std::function<std::string(const std::string &aafPath)> embeddedDir;
};

// Shows the dialog. Returns true when the user pressed Import; `opt` and the session's
// track flags / colors then hold the choices.
// the import window when one is open (one at a time), else nullptr
HWND openDialogHwnd();

// The start window: what OLAAF does, the AAFs opened before and the button to choose one.
// Returns the AAFs to open (several = reels), empty when it was closed.
std::vector<std::string> showStartWindow(HWND parent, const std::vector<std::string> &recent,
                                         std::function<std::vector<std::string>(HWND parent)> pick);

bool showImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host);
// The same window without blocking REAPER: returns at once; onClose runs after the window is closed
// (accepted = Build session). Only one can be open; a second call brings it to the front and returns nullptr.
HWND openImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host,
                      std::function<void(bool accepted)> onClose);

void dialogSetInstance(void *hInstance);
// for REAPER's accelerator hook ("accelerator" plugin_register): keys while the import dialog is open
int dialogTranslateKey(MSG *msg);

} // namespace olaaf
