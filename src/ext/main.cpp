// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// REAPER extension entry point.
//  * File > Open project / drag and drop of an .aaf  -> opens the AAF as a new REAPER project
//  * Action "OLAAF: Import AAF into current project..." -> adds the AAF tracks to the open project

// C++ headers first: SWELL (mac/linux) and windows.h define min/max macros
#include <algorithm>
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
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceLength
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_SetItemStateChunk
#define REAPERAPI_WANT_GetTrackEnvelopeByChunkName
#define REAPERAPI_WANT_DeleteEnvelopePointRange
#define REAPERAPI_WANT_InsertEnvelopePoint
#define REAPERAPI_WANT_Envelope_SortPoints
#define REAPERAPI_WANT_ScaleToEnvelopeMode
#define REAPERAPI_WANT_GetEnvelopeScalingMode
#define REAPERAPI_WANT_GetTrackStateChunk
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

// media folders the user located before (newest first); searched again on the next import
static std::vector<std::string> loadMediaFolders() {
  std::vector<std::string> out;
  const char *v = GetExtState("olAAF", "mediaFolders");
  std::string cur;
  for (const char *p = v ? v : ""; ; p++) {
    if (*p == '\n' || !*p) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      if (!*p) break;
    } else
      cur += *p;
  }
  return out;
}

static void saveMediaFolders(const std::vector<std::string> &folders) {
  std::vector<std::string> keep;
  for (auto &f : folders)
    if (keep.size() < 6 && std::find(keep.begin(), keep.end(), f) == keep.end()) keep.push_back(f);
  std::string v;
  for (auto &f : keep) v += (v.empty() ? "" : "\n") + f;
  SetExtState("olAAF", "mediaFolders", v.c_str(), true);
}

static void loadSettings(ImportOptions &o) {
  o.copyToSubdir = false; // always starts off: WAVs stay where they are unless asked for this import
  o.mergeStereo = getFlag("merge", true);
  o.fades = getFlag("fades", true);
  o.clipGain = getFlag("gain", true);
  o.clipGainToTrack = getFlag("gainToTrack", false);
  o.showAutomation = getFlag("showAuto", false);
  o.video = getFlag("video", true);
  o.markers = getFlag("markers", true);
  o.folders = getFlag("folders", true);
  setDarkTheme(getFlag("dark", false));
  const char *d = GetExtState("olAAF", "dest");
  if (d && *d && getFlag("destCustom", false)) o.destDir = d;
}

static void saveSettings(const ImportOptions &o, const std::string &defaultDest) {

  setFlag("merge", o.mergeStereo);
  setFlag("fades", o.fades);
  setFlag("gain", o.clipGain);
  setFlag("gainToTrack", o.clipGainToTrack);
  setFlag("showAuto", o.showAutomation);
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
  h.projectTracks = []() {
    std::vector<std::pair<std::string, std::string>> out;
    int n = CountTracks(nullptr);
    for (int i = 0; i < n; i++) {
      MediaTrack *tr = GetTrack(nullptr, i);
      char guid[64] = {0}, name[512] = {0};
      if (!tr || !GetSetMediaTrackInfo_String(tr, "GUID", guid, false)) continue;
      GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
      out.push_back({guid, name[0] ? std::string(name) : strprintf("Track %d", i + 1)});
    }
    return out;
  };
  return h;
}

// read the AAF (worker thread + progress window), then show the dialog, then write the media
// step 1: read the AAF and find its media (progress window), then the remembered settings
static bool loadForImport(const std::string &aaf, ImportOptions &opt, Importer &imp, std::string &err,
                          std::string &defaultDest) {
  bool cancelled = false;
  {
    // newest first: the first folder that has a file wins
    auto remembered = loadMediaFolders();
    opt.searchFolders.insert(opt.searchFolders.end(), remembered.begin(), remembered.end());
  }
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
  defaultDest = opt.destDir;
  loadSettings(opt);
  imp.setMergeStereo(opt.mergeStereo);
  return true;
}

// step 3 (after the dialog): write the media (progress window)
static bool finishImport(Importer &imp, ImportOptions &opt, MediaReport &rep, int &merged, std::string &err) {
  merged = 0;
  for (auto &t : imp.session().tracks) merged += !t.mergedFrom.empty();
  bool cancelled = false;
  bool ok = runWithProgress(GetMainHwnd(), [&](const Progress &progress) {
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

// all three steps with a modal dialog (File > Open project: REAPER waits for the result)
static bool runImport(const std::string &aaf, ImportOptions &opt, Importer &imp, MediaReport &rep, int &merged,
                      std::string &err) {
  std::string defaultDest;
  if (!loadForImport(aaf, opt, imp, err, defaultDest)) return false;
  DialogHost host = makeHost();
  host.projectTracks = nullptr; // a new project: no existing tracks to put the AAF on
  bool accepted = showImportDialog(GetMainHwnd(), imp, opt, host);
  saveSettings(opt, defaultDest);
  saveMediaFolders(opt.searchFolders); // Locate puts its folder first
  if (!accepted) {
    err.clear(); // closed without importing
    return false;
  }
  return finishImport(imp, opt, rep, merged, err);
}

// "Overwrite envelopes": the AAF's volume / pan automation replaces what the existing track has over the
// AAF's time range; outside it the track's own points stay. A track without such an envelope gets one.
static void overwriteOne(MediaTrack *tr, const char *chunkName, const std::vector<GainPoint> &pts,
                         const std::string &block, double t0, double t1, bool pan) {
  if (pts.empty()) return;
  TrackEnvelope *env = GetTrackEnvelopeByChunkName(tr, chunkName);
  if (env) {
    DeleteEnvelopePointRange(env, t0 - 0.001, t1 + 0.001);
    int mode = GetEnvelopeScalingMode(env);
    bool noSort = true;
    for (auto &p : pts) InsertEnvelopePoint(env, p.time, pan ? p.gain : ScaleToEnvelopeMode(mode, p.gain), 0, 0, false, &noSort);
    Envelope_SortPoints(env);
    return;
  }
  // no such envelope yet: put the block into the track's state chunk
  std::vector<char> buf(8 << 20);
  if (!GetTrackStateChunk(tr, buf.data(), (int)buf.size(), false)) return;
  std::string c = buf.data();
  size_t at = c.find("\n<ITEM");
  if (at == std::string::npos) at = c.rfind("\n>");
  if (at == std::string::npos) return;
  c.insert(at + 1, block);
  SetTrackStateChunk(tr, c.c_str(), false);
}

static void overwriteEnvelopes(MediaTrack *tr, const TrackChunk &tc) {
  overwriteOne(tr, "<VOLENV2", tc.volEnv, tc.volEnvChunk, tc.rangeStart, tc.rangeEnd, false);
  overwriteOne(tr, "<PANENV2", tc.panEnv, tc.panEnvChunk, tc.rangeStart, tc.rangeEnd, true);
}

static void insertTracks(Importer &imp, ImportOptions &opt, const MediaReport &rep, int merged) {
  Undo_BeginBlock2(nullptr);
  PreventUIRefresh(1);
  int base = CountTracks(nullptr);
  auto chunks = imp.trackChunks(opt);
  int added = 0;
  for (size_t i = 0; i < chunks.size(); i++) {
    if (!chunks[i].targetGuid.empty()) { // onto an existing track ("match tracks"): only the items
      MediaTrack *dst = nullptr;
      for (int k = 0; k < CountTracks(nullptr) && !dst; k++) {
        MediaTrack *tr = GetTrack(nullptr, k);
        char guid[64] = {0};
        if (tr && GetSetMediaTrackInfo_String(tr, "GUID", guid, false) && chunks[i].targetGuid == guid) dst = tr;
      }
      if (dst) {
        overwriteEnvelopes(dst, chunks[i]);
        for (auto &it : chunks[i].items) {
          MediaItem *mi = AddMediaItemToTrack(dst);
          if (mi) SetItemStateChunk(mi, it.c_str(), false);
        }
        continue;
      }
      // the track is gone (deleted while the window was open): a new track after all
    }
    InsertTrackAtIndex(base + added, false);
    MediaTrack *tr = GetTrack(nullptr, base + added);
    added++;
    if (!tr) continue;
    SetTrackStateChunk(tr, chunks[i].text.c_str(), false);
    // color and folder structure through the API, so they are right on every platform
    uint32_t c = chunks[i].color;
    if (c) SetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR", (double)(ColorToNative((c >> 16) & 255, (c >> 8) & 255, c & 255) | 0x1000000));
    SetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH", (double)chunks[i].folderDepth);
    if (chunks[i].videoFromFile) // a picture chosen by hand: the item as long as the file
      for (int k = 0; k < CountTrackMediaItems(tr); k++) {
        MediaItem *it = GetTrackMediaItem(tr, k);
        MediaItem_Take *tk = it ? GetActiveTake(it) : nullptr;
        PCM_source *src = tk ? GetMediaItemTake_Source(tk) : nullptr;
        bool qn = false;
        double len = src ? GetMediaSourceLength(src, &qn) : 0.0;
        if (len > 0 && !qn) SetMediaItemInfo_Value(it, "D_LENGTH", len);
      }
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
  Undo_EndBlock2(nullptr, "OLAAF: Import AAF", -1);

  ShowMessageBox(summary(imp, rep, merged).c_str(), "OLAAF", 0);
}

// the import into the open project: the window does not block REAPER (the edit cursor can be moved,
// the project scrolled) - the import runs when the window is closed with Build session
struct PendingImport {
  ImportOptions opt;
  Importer imp;
  DialogHost host;
  std::string defaultDest;
};

static void importIntoProject() {
  char fn[4096] = {0};
  if (!GetUserFileNameForRead(fn, "OLAAF: Import AAF", "aaf")) return;

  PendingImport *p = new PendingImport;
  char proj[4096] = {0};
  GetProjectPath(proj, sizeof(proj));
  p->opt.destDir = proj[0] ? proj : dirName(fn);
  p->host = makeHost();
  std::string err;
  if (!loadForImport(fn, p->opt, p->imp, err, p->defaultDest)) {
    if (!err.empty()) ShowMessageBox(err.c_str(), "OLAAF", 0);
    delete p;
    return;
  }
  HWND h = openImportDialog(GetMainHwnd(), p->imp, p->opt, p->host, [p](bool accepted) {
    saveSettings(p->opt, p->defaultDest);
    saveMediaFolders(p->opt.searchFolders);
    if (accepted) {
      MediaReport rep;
      int merged = 0;
      std::string e;
      if (finishImport(p->imp, p->opt, rep, merged, e)) insertTracks(p->imp, p->opt, rep, merged);
      else if (!e.empty()) ShowMessageBox(e.c_str(), "OLAAF", 0);
    }
    delete p;
  });
  if (!h) delete p; // one is open already (brought to the front)
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
  mi.dwTypeData = (char *)"OLAAF: Import AAF...";
  InsertMenuItem((HMENU)menu, GetMenuItemCount((HMENU)menu), TRUE, &mi);
}

// ---------------------------------------------------------------- project import (.aaf as project)

static bool wantProjectFile(const char *fn) {
  return fn && lower(fileExt(fn)) == "aaf";
}

static const char *enumFileExtensions(int i, char **desc) {
  if (i == 0) {
    if (desc) *desc = (char *)"AAF projects (OLAAF)";
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
    if (!err.empty()) ShowMessageBox(err.c_str(), "OLAAF", 0);
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
  ShowMessageBox(summary(imp, rep, merged).c_str(), "OLAAF", 0);
  return 0;
}

static project_import_register_t g_projImport = {wantProjectFile, enumFileExtensions, loadProject};

// ---------------------------------------------------------------- entry

static int translateAccel(MSG *msg, accelerator_register_t *) { return olaaf::dialogTranslateKey(msg); }
static accelerator_register_t g_accel = {translateAccel, true, nullptr};

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t *rec) {
  if (!rec) {
    plugin_register("-hookcommand2", (void *)onAction);
    plugin_register("-projectimport", &g_projImport);
    plugin_register("-hookcustommenu", (void *)menuHook);
    plugin_register("-accelerator", &g_accel);
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;
  if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;
  progressSetInstance((void *)hInstance);
  dialogSetInstance((void *)hInstance);

  static custom_action_register_t act = {0, "DMAJORSOUND_OLAAF_IMPORT", "OLAAF: Import AAF into current project...", nullptr};
  g_cmdImport = plugin_register("custom_action", &act);
  plugin_register("hookcommand2", (void *)onAction);
  plugin_register("projectimport", &g_projImport);
  AddExtensionsMainMenu();
  plugin_register("hookcustommenu", (void *)menuHook);
  plugin_register("accelerator", &g_accel);
  return 1;
}
