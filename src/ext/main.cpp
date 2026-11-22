// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// REAPER extension entry point.
//  * File > Open project / drag and drop of an .aaf  -> opens the AAF as a new REAPER project
//  * Action "olAAF: Import AAF into current project..." -> adds the AAF tracks to the open project

// C++ headers first: SWELL (mac/linux) and windows.h define min/max macros
#include "../core/importer.h"
#include "../core/util.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ColorToNative
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetProjectPath
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetUserFileNameForRead
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_PreventUIRefresh
#define REAPERAPI_WANT_SetMediaTrackInfo_Value
#define REAPERAPI_WANT_SetTrackStateChunk
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_TrackList_AdjustWindows
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_plugin_register
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
#undef min
#undef max

using namespace olaaf;

static int g_cmdImport = 0;

static std::string summary(const Importer &imp, const MediaReport &rep, int merged) {
  const Session &s = const_cast<Importer &>(imp).session();
  int clips = 0, tracks = 0;
  for (auto &t : s.tracks)
    if (t.import) {
      tracks++;
      clips += (int)t.clips.size();
    }
  std::string m = strprintf("%s\n%s %s\n\n%d tracks, %d clips", s.name.c_str(), s.product.c_str(), s.version.c_str(),
                            tracks, clips);
  if (merged) m += strprintf(", %d .L/.R pairs merged to stereo", merged);
  m += "\n";
  if (rep.extracted) m += strprintf("%d embedded files extracted\n", rep.extracted);
  if (rep.converted) m += strprintf("%d files converted to Broadcast WAV\n", rep.converted);
  if (rep.copied) m += strprintf("%d WAV files copied\n", rep.copied);
  if (rep.interleaved) m += strprintf("%d multichannel files combined\n", rep.interleaved);
  if (rep.missing) {
    m += strprintf("\n%d files are missing and show as offline items:\n", rep.missing);
    for (size_t i = 0; i < rep.missingNames.size() && i < 8; i++) m += "  " + rep.missingNames[i] + "\n";
    if (rep.missingNames.size() > 8) m += strprintf("  ... and %d more\n", (int)rep.missingNames.size() - 8);
  }
  int shown = 0;
  for (auto &w : s.warnings) {
    if (w.rfind("AAF: ", 0) == 0) continue; // parser noise
    if (!shown++) m += "\nNotes:\n";
    if (shown > 8) { m += "  ...\n"; break; }
    m += "  " + w + "\n";
  }
  return m;
}

static bool runImport(const std::string &aaf, ImportOptions &opt, Importer &imp, MediaReport &rep, int &merged,
                      std::string &err) {
  if (!imp.load(aaf, opt, &err)) return false;
  imp.locateMedia(opt);
  merged = opt.mergeStereo ? imp.mergeStereoTracks() : 0;
  imp.prepareMedia(opt, &rep, nullptr);
  return true;
}

// ---------------------------------------------------------------- action: import into current project

static void importIntoProject() {
  char fn[4096] = {0};
  if (!GetUserFileNameForRead(fn, "olAAF: Import AAF", "aaf")) return;

  ImportOptions opt;
  char proj[4096] = {0};
  GetProjectPath(proj, sizeof(proj));
  opt.destDir = proj[0] ? proj : dirName(fn);

  Importer imp;
  MediaReport rep;
  int merged = 0;
  std::string err;
  if (!runImport(fn, opt, imp, rep, merged, err)) {
    ShowMessageBox(err.c_str(), "olAAF", 0);
    return;
  }

  Undo_BeginBlock2(nullptr);
  PreventUIRefresh(1);
  int base = CountTracks(nullptr);
  auto chunks = imp.trackChunks(opt);
  size_t ti = 0;
  for (size_t i = 0; i < chunks.size(); i++) {
    InsertTrackAtIndex(base + (int)i, false);
    MediaTrack *tr = GetTrack(nullptr, base + (int)i);
    if (!tr) continue;
    SetTrackStateChunk(tr, chunks[i].c_str(), false);
    // set the color through the API so it is right on every platform
    while (ti < imp.session().tracks.size() && !imp.session().tracks[ti].import) ti++;
    if (ti < imp.session().tracks.size()) {
      uint32_t c = imp.session().tracks[ti].color;
      if (c) SetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR", (double)(ColorToNative((c >> 16) & 255, (c >> 8) & 255, c & 255) | 0x1000000));
      ti++;
    }
  }
  PreventUIRefresh(-1);
  TrackList_AdjustWindows(false);
  UpdateArrange();
  Undo_EndBlock2(nullptr, "olAAF: Import AAF", -1);

  ShowMessageBox(summary(imp, rep, merged).c_str(), "olAAF", 0);
}

static bool onAction(KbdSectionInfo *sec, int command, int val, int val2, int relmode, HWND hwnd) {
  (void)sec; (void)val; (void)val2; (void)relmode; (void)hwnd;
  if (command == g_cmdImport && g_cmdImport) {
    importIntoProject();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- project import (.aaf as project)

static bool wantProjectFile(const char *fn) {
  return fn && lower(fileExt(fn)) == "aaf";
}

static const char *enumFileExtensions(int i, char **desc) {
  if (i == 0) {
    if (desc) *desc = (char *)"AAF projects (olAAF)";
    return "AAF";
  }
  if (desc) *desc = nullptr;
  return nullptr;
}

static int loadProject(const char *fn, ProjectStateContext *ctx) {
  ImportOptions opt;
  opt.destDir = dirName(fn);
  Importer imp;
  MediaReport rep;
  int merged = 0;
  std::string err;
  if (!runImport(fn, opt, imp, rep, merged, err)) {
    ShowMessageBox(err.c_str(), "olAAF", 0);
    return -1;
  }
  std::string rpp = imp.projectRpp(opt);
  size_t p = 0;
  while (p < rpp.size()) {
    size_t e = rpp.find('\n', p);
    if (e == std::string::npos) e = rpp.size();
    std::string line = rpp.substr(p, e - p);
    ctx->AddLine("%s", line.c_str());
    p = e + 1;
  }
  if (rep.missing) ShowMessageBox(summary(imp, rep, merged).c_str(), "olAAF", 0);
  return 0;
}

static project_import_register_t g_projImport = {wantProjectFile, enumFileExtensions, loadProject};

// ---------------------------------------------------------------- entry

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t *rec) {
  (void)hInstance;
  if (!rec) {
    plugin_register("-hookcommand2", (void *)onAction);
    plugin_register("-projectimport", &g_projImport);
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;
  if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

  static custom_action_register_t act = {0, "DMAJORSOUND_OLAAF_IMPORT", "olAAF: Import AAF into current project...", nullptr};
  g_cmdImport = plugin_register("custom_action", &act);
  plugin_register("hookcommand2", (void *)onAction);
  plugin_register("projectimport", &g_projImport);
  return 1;
}
