// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// REAPER extension entry point.
//  * File > Open project / drag and drop of an .aaf  -> opens the AAF as a new REAPER project
//  * Action "olAAF: Import AAF into current project..." -> adds the AAF tracks to the open project

// C++ headers first: SWELL (mac/linux) and windows.h define min/max macros
#include "../core/importer.h"
#include "../core/util.h"
#include "progress.h"
#include "dialog.h"
#include "theme.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_AddExtensionsMainMenu
#define REAPERAPI_WANT_ColorToNative
#define REAPERAPI_WANT_ColorFromNative
#define REAPERAPI_WANT_GR_SelectColor
#define REAPERAPI_WANT_GetUserInputs
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetProjectPath
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetUserFileNameForRead
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_AddProjectMarker2
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_Main_OnCommand
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
  if (!rep.logPath.empty()) m += "\nDetails: " + rep.logPath + "\n";
  int shown = 0;
  for (auto &w : s.warnings) {
    if (w.rfind("AAF: ", 0) == 0) continue; // parser noise
    if (!shown++) m += "\nNotes:\n";
    if (shown > 8) { m += "  ...\n"; break; }
    m += "  " + w + "\n";
  }
  return m;
}

// ---------------------------------------------------------------- remembered settings

static bool getFlag(const char *key, bool def) {
  const char *v = GetExtState("olAAF", key);
  return (v && *v) ? v[0] == '1' : def;
}
static void setFlag(const char *key, bool v) { SetExtState("olAAF", key, v ? "1" : "0", true); }

static void loadSettings(ImportOptions &o) {
  o.copyToSubdir = getFlag("copy", true);
  o.mergeStereo = getFlag("merge", true);
  o.fades = getFlag("fades", true);
  o.clipGain = getFlag("gain", true);
  o.video = getFlag("video", true);
  o.markers = getFlag("markers", true);
  o.folders = getFlag("folders", true);
  setDarkTheme(getFlag("dark", false));
  const char *d = GetExtState("olAAF", "dest");
  if (d && *d && getFlag("destCustom", false)) o.destDir = d;
}

static void saveSettings(const ImportOptions &o, const std::string &defaultDest) {
  setFlag("copy", o.copyToSubdir);
  setFlag("merge", o.mergeStereo);
  setFlag("fades", o.fades);
  setFlag("gain", o.clipGain);
  setFlag("video", o.video);
  setFlag("markers", o.markers);
  setFlag("folders", o.folders);
  setFlag("dark", darkTheme());
  // remember a destination only when the user chose one (not the project folder)
  bool custom = o.destDir != defaultDest;
  setFlag("destCustom", custom);
  if (custom) SetExtState("olAAF", "dest", o.destDir.c_str(), true);
}

static DialogHost makeHost() {
  DialogHost h;
  h.askText = [](const char *title, const char *caption, std::string *value) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", value->c_str());
    std::string cap = std::string(caption) + ",extrawidth=260,separator=\x1f";
    if (!GetUserInputs(title, 1, cap.c_str(), buf, sizeof(buf))) return false;
    *value = buf;
    return true;
  };
  h.pickColor = [](HWND parent, uint32_t *rgb) {
    int c = ColorToNative((*rgb >> 16) & 255, (*rgb >> 8) & 255, *rgb & 255);
    if (!GR_SelectColor(parent, &c)) return false;
    int r = 0, g = 0, b = 0;
    ColorFromNative(c, &r, &g, &b);
    *rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
  };
  h.pickFile = [](const char *title, std::string *path) {
    char fn[4096] = {0};
    if (!GetUserFileNameForRead(fn, title, "")) return false;
    *path = fn;
    return true;
  };
  h.editCursor = []() { return GetCursorPosition(); };
  return h;
}

// read the AAF (worker thread + progress window), then show the dialog, then write the media
static bool runImport(const std::string &aaf, ImportOptions &opt, Importer &imp, MediaReport &rep, int &merged,
                      std::string &err) {
  bool cancelled = false;
  bool ok = runWithProgress(GetMainHwnd(), [&](const Progress &progress) {
    progress(0.0, "Reading " + baseName(aaf));
    if (!imp.load(aaf, opt, &err)) return false;
    progress(0.6, "Looking for linked media");
    imp.locateMedia(opt);
    imp.setMergeStereo(opt.mergeStereo);
    return true;
  }, &cancelled);
  if (!ok || cancelled) {
    if (cancelled) err = "Import cancelled.";
    return false;
  }

  std::string defaultDest = opt.destDir;
  loadSettings(opt);
  imp.setMergeStereo(opt.mergeStereo);
  bool accepted = showImportDialog(GetMainHwnd(), imp, opt, makeHost());
  saveSettings(opt, defaultDest);
  if (!accepted) {
    err.clear(); // closed without importing
    return false;
  }
  merged = 0;
  for (auto &t : imp.session().tracks) merged += !t.mergedFrom.empty();

  ok = runWithProgress(GetMainHwnd(), [&](const Progress &progress) {
    if (!imp.prepareMedia(opt, &rep, progress)) return false;
    progress(1.0, "Building your Reaper session");
    return true;
  }, &cancelled);
  if (cancelled) {
    err = "Import cancelled. Files that were already written stay in the audio folder.";
    return false;
  }
  return ok;
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
    if (!err.empty()) ShowMessageBox(err.c_str(), "olAAF", 0);
    return;
  }

  Undo_BeginBlock2(nullptr);
  PreventUIRefresh(1);
  int base = CountTracks(nullptr);
  auto chunks = imp.trackChunks(opt);
  for (size_t i = 0; i < chunks.size(); i++) {
    InsertTrackAtIndex(base + (int)i, false);
    MediaTrack *tr = GetTrack(nullptr, base + (int)i);
    if (!tr) continue;
    SetTrackStateChunk(tr, chunks[i].text.c_str(), false);
    // color and folder structure through the API, so they are right on every platform
    uint32_t c = chunks[i].color;
    if (c) SetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR", (double)(ColorToNative((c >> 16) & 255, (c >> 8) & 255, c & 255) | 0x1000000));
    SetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH", (double)chunks[i].folderDepth);
  }
  if (opt.markers)
    for (auto &m : imp.session().markers) {
      uint32_t c = m.color;
      int col = c ? (ColorToNative((c >> 16) & 255, (c >> 8) & 255, c & 255) | 0x1000000) : 0;
      std::string name = m.name.empty() ? m.comment : m.name;
      AddProjectMarker2(nullptr, m.len > 0, m.pos + opt.tcOffset, m.pos + m.len + opt.tcOffset, name.c_str(), -1, col);
    }
  PreventUIRefresh(-1);
  TrackList_AdjustWindows(false);
  // build the waveforms right away instead of when the items are first drawn
  Main_OnCommand(40047, 0); // Peaks: Build any missing peaks
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

// ---------------------------------------------------------------- Extensions menu

static void menuHook(const char *menuId, void *menu, int flag) {
  if (flag != 0 || !menu || !g_cmdImport || strcmp(menuId, "Main extensions")) return;
  MENUITEMINFO mi;
  memset(&mi, 0, sizeof(mi));
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_TYPE | MIIM_ID;
  mi.fType = MFT_STRING;
  mi.wID = (UINT)g_cmdImport;
  mi.dwTypeData = (char *)"olAAF: Import AAF...";
  InsertMenuItem((HMENU)menu, GetMenuItemCount((HMENU)menu), TRUE, &mi);
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
    if (!err.empty()) ShowMessageBox(err.c_str(), "olAAF", 0);
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
  ShowMessageBox(summary(imp, rep, merged).c_str(), "olAAF", 0);
  return 0;
}

static project_import_register_t g_projImport = {wantProjectFile, enumFileExtensions, loadProject};

// ---------------------------------------------------------------- entry

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t *rec) {
  if (!rec) {
    plugin_register("-hookcommand2", (void *)onAction);
    plugin_register("-projectimport", &g_projImport);
    plugin_register("-hookcustommenu", (void *)menuHook);
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;
  if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;
  progressSetInstance((void *)hInstance);
  dialogSetInstance((void *)hInstance);

  static custom_action_register_t act = {0, "DMAJORSOUND_OLAAF_IMPORT", "olAAF: Import AAF into current project...", nullptr};
  g_cmdImport = plugin_register("custom_action", &act);
  plugin_register("hookcommand2", (void *)onAction);
  plugin_register("projectimport", &g_projImport);
  AddExtensionsMainMenu();
  plugin_register("hookcustommenu", (void *)menuHook);
  return 1;
}
