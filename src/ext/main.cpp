// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// REAPER extension entry point.
//  * File > Open project / drag and drop of an .aaf  -> opens the AAF as a new REAPER project
//  * Action "OLAAF: Import AAF into current project..." -> adds the AAF tracks to the open project

// C++ headers first: SWELL (mac/linux) and windows.h define min/max macros
#include <algorithm>
#include "../core/audiofile.h"
#include "../core/importer.h"
#include "../core/peaks.h"
#ifdef __APPLE__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#include "../core/report.h"
#include "../core/track_template.h"
#include "../core/reels.h"
#include <memory>
#include "../core/util.h"
#include "progress.h"
#include "dialog.h"
#include "theme.h"
#include "update.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
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
#define REAPERAPI_WANT_GetThemeColor
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_GetSetProjectInfo
#define REAPERAPI_WANT_GetAudioDeviceInfo
#define REAPERAPI_WANT_PCM_Source_CreateFromFileEx
#define REAPERAPI_WANT_PlayPreview
#define REAPERAPI_WANT_StopPreview
#define REAPERAPI_WANT_TimeMap_curFrameRate
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_GetResourcePath
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
  if (!rep.logPath.empty()) m += "\nImport report: " + rep.logPath + "\n";
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
  o.showAutomation = false; // the automation lanes start hidden on every import (the button shows them)
  o.video = getFlag("video", true);
  o.markers = getFlag("markers", true);
  o.folders = getFlag("folders", true);
  const char *d = GetExtState("olAAF", "dest");
  if (d && *d && getFlag("destCustom", false)) o.destDir = d;
}

static void saveSettings(const ImportOptions &o, const std::string &defaultDest) {

  setFlag("merge", o.mergeStereo);
  setFlag("fades", o.fades);
  setFlag("gain", o.clipGain);
  setFlag("gainToTrack", o.clipGainToTrack);
  setFlag("video", o.video);
  setFlag("markers", o.markers);
  setFlag("folders", o.folders);
  // remember a destination only when the user chose one (not the project folder)
  bool custom = o.destDir != defaultDest;
  setFlag("destCustom", custom);
  if (custom) SetExtState("olAAF", "dest", o.destDir.c_str(), true);
}

// the whole timeline as one source: every event read from its file and mixed to stereo
class TimelineSource : public PCM_source {
public:
  struct Ev {
    int file;
    MixEvent e;
  };
  TimelineSource(const std::vector<MixEvent> &events, double end) : m_end(end) {
    std::vector<std::string> paths;
    for (auto &e : events) {
      int fi = -1;
      for (size_t k = 0; k < paths.size(); k++)
        if (paths[k] == e.path) fi = (int)k;
      if (fi < 0) {
        PCM_source *src = PCM_Source_CreateFromFileEx(e.path.c_str(), true);
        if (!src) continue; // REAPER can't read it (it is converted on import)
        paths.push_back(e.path);
        m_files.push_back(src);
        fi = (int)m_files.size() - 1;
      }
      m_ev.push_back({fi, e});
    }
  }
  ~TimelineSource() override {
    for (auto *f : m_files) delete f;
  }
  bool empty() const { return m_ev.empty(); }
  PCM_source *Duplicate() override { return nullptr; }
  bool IsAvailable() override { return true; }
  const char *GetType() override { return "OLAAFMIX"; }
  bool SetFileName(const char *) override { return false; }
  int GetNumChannels() override { return 2; }
  double GetSampleRate() override { return 48000.0; }
  double GetLength() override { return m_end; }
  int PropertiesWindow(HWND) override { return -1; }
  void GetPeakInfo(PCM_source_peaktransfer_t *b) override { b->peaks_out = 0; }
  void SaveState(ProjectStateContext *) override {}
  int LoadState(const char *, ProjectStateContext *) override { return -1; }
  void Peaks_Clear(bool) override {}
  int PeaksBuild_Begin() override { return 0; }
  int PeaksBuild_Run() override { return 0; }
  void PeaksBuild_Finish() override {}

  void GetSamples(PCM_source_transfer_t *b) override {
    int nch = b->nch, len = b->length;
    double sr = b->samplerate, t0 = b->time_s;
    if (nch <= 0 || len <= 0 || sr <= 0) return;
    for (int i = 0; i < nch * len; i++) b->samples[i] = 0;
    double t1 = t0 + len / sr;
    for (auto &ev : m_ev) {
      const MixEvent &e = ev.e;
      if (e.pos >= t1 || e.pos + e.len <= t0) continue;
      int a = std::max(0, (int)std::ceil((e.pos - t0) * sr));
      int z = std::min(len, (int)std::floor((e.pos + e.len - t0) * sr));
      if (z <= a) continue;
      PCM_source *src = m_files[(size_t)ev.file];
      int cn = std::max(1, std::min(8, src->GetNumChannels()));
      m_buf.resize((size_t)(z - a) * cn);
      PCM_source_transfer_t tr{};
      tr.time_s = e.offset + (t0 + a / sr - e.pos);
      tr.samplerate = sr;
      tr.nch = cn;
      tr.length = z - a;
      tr.samples = m_buf.data();
      src->GetSamples(&tr);
      for (int f = 0; f < tr.samples_out && f < z - a; f++) {
        double t = t0 + (a + f) / sr - e.pos; // into the event
        double g = e.gain;
        if (e.fadeIn > 0 && t < e.fadeIn) g *= t / e.fadeIn;
        if (e.fadeOut > 0 && e.len - t < e.fadeOut) g *= std::max(0.0, (e.len - t) / e.fadeOut);
        const ReaSample *in = m_buf.data() + (size_t)f * cn;
        double l, r;
        if (e.side == 1) l = in[0], r = 0;
        else if (e.side == 2) l = 0, r = in[0];
        else if (e.side == 3 && cn >= 2) l = in[1], r = in[0]; // left and right exchanged
        else if (cn == 1) l = r = in[0];
        else l = in[0], r = in[1];
        ReaSample *out = b->samples + (size_t)(a + f) * nch;
        out[0] += l * g;
        if (nch > 1) out[1] += r * g;
      }
    }
    b->samples_out = len;
  }

private:
  std::vector<PCM_source *> m_files;
  std::vector<Ev> m_ev;
  std::vector<ReaSample> m_buf;
  double m_end;
};

// ------------------------------------------------------------------ listening (REAPER preview)
// Up to two files at once (an L / R pair of mono files); stopped at the end of the event.
namespace preview {
struct Voice {
  preview_register_t reg{};
  bool on = false;
};
static Voice g_voice[2];
static double g_start = 0, g_len = 0;
static bool g_timeline = false; // pos() reports the timeline position

static void lockInit(preview_register_t &r) {
#ifdef _WIN32
  InitializeCriticalSection(&r.cs);
#else
  pthread_mutex_init(&r.mutex, NULL);
#endif
}
static void lockFree(preview_register_t &r) {
#ifdef _WIN32
  DeleteCriticalSection(&r.cs);
#else
  pthread_mutex_destroy(&r.mutex);
#endif
}

static void stop() {
  for (auto &v : g_voice) {
    if (!v.on) continue;
    StopPreview(&v.reg);
    delete v.reg.src;
    lockFree(v.reg);
    v.reg = preview_register_t();
    v.on = false;
  }
}

static bool playMix(TimelineSource *mix, double start, bool timeline) {
  stop();
  if (mix->empty()) {
    delete mix;
    return false;
  }
  Voice &v = g_voice[0];
  lockInit(v.reg);
  v.reg.src = mix;
  v.reg.m_out_chan = 0;
  v.reg.curpos = start;
  v.reg.loop = false;
  v.reg.volume = 1.0;
  v.on = true;
  g_timeline = timeline;
  PlayPreview(&v.reg);
  return true;
}

// one event: its parts (one file, or the two files of an L / R pair) as a little mix from 0
static bool play(const std::vector<PreviewPart> &parts) {
  if (parts.empty()) return false;
  std::vector<MixEvent> evs;
  for (auto &p : parts) {
    MixEvent e;
    e.path = p.path;
    e.pos = 0;
    e.len = p.length;
    e.offset = p.offset;
    e.gain = p.gain;
    e.side = p.side;
    evs.push_back(e);
  }
  g_start = 0;
  g_len = parts[0].length;
  return playMix(new TimelineSource(evs, parts[0].length), 0, false);
}

// seconds played, -1 when stopped (at the end of the event it stops by itself)
static double pos() {
  if (!g_voice[0].on) return -1;
  preview_register_t &r = g_voice[0].reg;
#ifdef _WIN32
  EnterCriticalSection(&r.cs);
  double c = r.curpos;
  LeaveCriticalSection(&r.cs);
#else
  pthread_mutex_lock(&r.mutex);
  double c = r.curpos;
  pthread_mutex_unlock(&r.mutex);
#endif
  if (g_timeline) {
    if (r.src && c >= r.src->GetLength()) {
      stop();
      return -1;
    }
    return c;
  }
  double played = c - g_start;
  bool done = played >= g_len;
  if (r.src) {
    double srcLen = r.src->GetLength();
    if (srcLen > 0 && c >= srcLen - 0.001) done = true;
  }
  if (done) {
    stop();
    return -1;
  }
  return std::max(0.0, played);
}
static bool playTimeline(const std::vector<MixEvent> &events, double start, double end) {
  return playMix(new TimelineSource(events, end), start, true);
}
} // namespace preview

// the sample rate the REAPER project runs at: its own, or the audio device's
static double projectSampleRate() {
  if (GetSetProjectInfo(nullptr, "PROJECT_SRATE_USE", 0, false) >= 0.5) return GetSetProjectInfo(nullptr, "PROJECT_SRATE", 0, false);
  char buf[64] = "";
  if (GetAudioDeviceInfo("SRATE", buf, sizeof(buf))) return atof(buf);
  return 0.0;
}

// A readable report of the import, next to the REAPER project (or in the audio folder while the
// project is not saved yet). Returns its path.
static std::string writeReport(Importer &imp, const ImportOptions &opt, const MediaReport &rep, int merged) {
  char rpp[4096] = "";
  EnumProjects(-1, rpp, sizeof(rpp));
  bool drop = false;
  double pf = TimeMap_curFrameRate(nullptr, &drop);
  return writeImportReport(imp.session(), opt, rep, merged, pf, projectSampleRate(), rpp, OLAAF_VERSION_STRING);
}

static void copyTextToClipboard(HWND hwnd, const std::string &text) {
#ifdef __APPLE__
  // macOS: pbcopy puts it on the general pasteboard as text (the SWELL clipboard didn't reach other apps)
  int fd[2];
  if (pipe(fd) == 0) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[0], 0);
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    const char *tool = "/usr/bin/pbcopy";
    char *argv[] = {(char *)tool, NULL};
    pid_t pid;
    bool ok = posix_spawn(&pid, tool, &fa, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(fd[0]);
    if (ok) {
      size_t off = 0;
      while (off < text.size()) {
        ssize_t w = write(fd[1], text.data() + off, text.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
      }
      close(fd[1]);
      int stt;
      waitpid(pid, &stt, 0);
      if (off == text.size()) return;
    } else
      close(fd[1]);
  }
#endif
  if (!OpenClipboard(hwnd)) return;
  EmptyClipboard();
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
  if (h) {
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, (wchar_t *)GlobalLock(h), n);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
#else
  HANDLE h = GlobalAlloc(GMEM_MOVEABLE, (int)text.size() + 1);
  if (h) {
    memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);
  }
#endif
  CloseClipboard();
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
  h.projectFps = [](bool *drop) { return TimeMap_curFrameRate(nullptr, drop); };
  h.projectSrate = []() { return projectSampleRate(); };
  h.playPreview = [](const std::vector<PreviewPart> &p) { return preview::play(p); };
  h.stopPreview = []() { preview::stop(); };
  h.previewPos = []() { return preview::pos(); };
  h.playTimeline = [](const std::vector<MixEvent> &e, double a, double b) { return preview::playTimeline(e, a, b); };
  h.updateState = [](std::string *tag, std::string *url) { return updateCheckState(tag, url); };
  h.openUrl = [](const std::string &url) { openWebPage(url); };
  h.revealFile = [](const std::string &path) { revealInFileBrowser(path); };
  h.copyText = [](HWND hwnd, const std::string &text) { copyTextToClipboard(hwnd, text); };
  // waveforms already read stay on disk: <REAPER resource folder>/OLAAF/peaks
  PeakCache::setDiskFolder(joinPath(GetResourcePath(), "OLAAF/peaks"));
  // embedded audio unpacked for the dialog: <REAPER resource folder>/OLAAF/embedded/<AAF>, the newest 5 GB stay
  trimSubfolders(joinPath(GetResourcePath(), "OLAAF/embedded"), 5000ull << 20);
  h.embeddedDir = [](const std::string &aafPath) {
    return joinPath(joinPath(GetResourcePath(), "OLAAF/embedded"), embeddedCacheName(aafPath));
  };
  // the choices per AAF: <REAPER resource folder>/OLAAF/choices/<name>
  h.loadChoices = [](const std::string &name) {
    std::string text;
    if (FILE *f = openUtf8(joinPath(joinPath(GetResourcePath(), "OLAAF/choices"), name), "rb")) {
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
      fclose(f);
    }
    return text;
  };
  h.saveChoices = [](const std::string &name, const std::string &text) {
    std::string dir = joinPath(GetResourcePath(), "OLAAF/choices");
    makeDirs(dir);
    if (FILE *f = openUtf8(joinPath(dir, name), "wb")) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
    }
  };
  h.getSetting = [](const char *key) {
    const char *v = GetExtState("olAAF", key);
    return std::string(v ? v : "");
  };
  h.setSetting = [](const char *key, const std::string &v) { SetExtState("olAAF", key, v.c_str(), true); };
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

// the look chosen under the OLAAF logo (system = REAPER's colors, dark or light), and the update check
static void applyLook() {
  updateCheckStart(); // is there a newer OLAAF? (once per session, in the background)
  const char *v = GetExtState("olAAF", "theme");
  setThemeMode(v && !strcmp(v, "light") ? 2 : v && !strcmp(v, "dark") ? 1 : 0);
  setThemeFromReaper([](const char *key) { return GetThemeColor(key, 0); });
}

// the AAFs opened before (start window), newest first
static std::vector<std::string> loadRecent() {
  std::vector<std::string> out;
  const char *r = GetExtState("olAAF", "recent");
  if (!r) return out;
  std::string all = r;
  size_t a = 0;
  while (a < all.size()) {
    size_t b = all.find('\n', a);
    if (b == std::string::npos) b = all.size();
    if (b > a) out.push_back(all.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

static void addRecent(const std::string &path) {
  std::vector<std::string> r = loadRecent();
  r.erase(std::remove_if(r.begin(), r.end(), [&](const std::string &p) { return lower(p) == lower(path); }), r.end());
  r.insert(r.begin(), path);
  if (r.size() > 6) r.resize(6);
  std::string all;
  for (auto &p : r) all += p + "\n";
  SetExtState("olAAF", "recent", all.c_str(), true);
}

// read the AAF (worker thread + progress window), then show the dialog, then write the media
// step 1: read the AAF and find its media (progress window), then the remembered settings
static bool loadForImport(const std::string &aaf, ImportOptions &opt, Importer &imp, std::string &err,
                          std::string &defaultDest) {
  applyLook();
  { // the folder sort rules (edited in the dialog)
    const char *r = GetExtState("olAAF", "rules");
    setSortRules(r && *r ? sortRulesFromText(r) : std::vector<SortRule>());
  }
  { // the track template added on import (chosen in the dialog)
    const char *t = GetExtState("olAAF", "template");
    opt.trackTemplate = t ? t : "";
  }
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

// ---------------------------------------------------------------- reels (several AAFs in one import)

struct ReelSet {
  Importer *first = nullptr;
  ImportOptions *firstOpt = nullptr;
  std::vector<std::unique_ptr<Importer>> imps;
  std::vector<std::unique_ptr<ImportOptions>> opts;
  bool firstOff = false; // the first AAF's tab was closed: it is not imported (it stays loaded underneath)
  int count() const { return 1 + (int)imps.size(); }
  Importer &imp(int i) { return i == 0 ? *first : *imps[(size_t)i - 1]; }
  ImportOptions &opt(int i) { return i == 0 ? *firstOpt : *opts[(size_t)i - 1]; }
};

// AAFs from the file dialog, several at once (GetOpenFileName style: one path, or folder + names)
static std::vector<std::string> pickAafs(HWND parent, bool first = false) {
  std::vector<std::string> out;
#ifdef _WIN32
  std::vector<wchar_t> buf(65536, 0);
  OPENFILENAMEW ofn;
  memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = parent;
  ofn.lpstrFilter = L"AAF files (*.aaf)\0*.aaf\0All files\0*.*\0\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.lpstrTitle = first ? L"OLAAF: choose the AAF (several reels: choose them all)" : L"OLAAF: add reels (choose one or more AAFs)";
  ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetOpenFileNameW(&ofn)) return out;
  auto utf8 = [](const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string r((size_t)std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &r[0], n, NULL, NULL);
    return r;
  };
  std::vector<std::string> parts;
  for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) parts.push_back(utf8(p));
#else
  (void)parent;
  char *res = BrowseForFiles(first ? "OLAAF: choose the AAF (several reels: choose them all)" : "OLAAF: add reels (choose one or more AAFs)",
                             NULL, NULL, true, "AAF files (*.aaf)\0*.aaf\0\0");
  if (!res) return out;
  std::vector<std::string> parts;
  for (const char *p = res; *p; p += strlen(p) + 1) parts.push_back(p);
  free(res);
#endif
  if (parts.size() == 1) out.push_back(parts[0]);
  else
    for (size_t i = 1; i < parts.size(); i++) out.push_back(joinPath(parts[0], parts[i]));
  return out;
}

// more AAFs into the import, each read with the progress window; returns how many were added
static int loadReelFiles(ReelSet *rs, HWND parent, const std::vector<std::string> &files, std::string *message) {
  int added = 0;
  std::vector<std::string> skipped, failed;
  for (auto &f : files) {
    bool dup = false;
    for (int i = 0; i < rs->count(); i++) dup |= lower(rs->imp(i).session().aafPath) == lower(f);
    if (dup) {
      skipped.push_back(baseName(f));
      continue;
    }
    std::unique_ptr<Importer> imp(new Importer);
    std::unique_ptr<ImportOptions> opt(new ImportOptions(*rs->firstOpt)); // the same options, the same offset
    std::string err;
    bool cancelled = false;
    bool ok = runWithProgress(parent, [&](const Progress &progress) {
      progress(0.0, "Reading " + baseName(f));
      if (!imp->load(f, *opt, &err)) return false;
      progress(0.6, "Looking for linked media");
      imp->locateMedia(*opt);
      imp->setMergeStereo(opt->mergeStereo);
      return true;
    }, &cancelled);
    if (cancelled) break;
    if (!ok) {
      failed.push_back(baseName(f) + (err.empty() ? std::string() : ": " + err));
      continue;
    }
    rs->imps.push_back(std::move(imp));
    rs->opts.push_back(std::move(opt));
    added++;
  }
  message->clear();
  if (!skipped.empty()) *message += "Already in this import: " + skipped[0] + (skipped.size() > 1 ? " and more." : ".") + " ";
  if (!failed.empty()) *message += "Not added: " + failed[0] + (failed.size() > 1 ? " (and more)." : ".");
  return added;
}

static void attachReels(DialogHost &h, ReelSet *rs) {
  h.reelCount = [rs]() { return rs->count(); };
  h.reelAt = [rs](int i, Importer **imp, ImportOptions **opt) {
    if (i < 0 || i >= rs->count()) return false;
    *imp = &rs->imp(i);
    *opt = &rs->opt(i);
    return true;
  };
  h.reelHidden = [rs](int i) { return i == 0 && rs->firstOff; };
  h.removeReel = [rs](int i) {
    if (i == 0) { // the first AAF holds the window's own data: it is only left out
      rs->firstOff = true;
      return;
    }
    if (i < 1 || i >= rs->count()) return;
    rs->imps.erase(rs->imps.begin() + (i - 1));
    rs->opts.erase(rs->opts.begin() + (i - 1));
  };
  h.addReels = [rs](HWND parent, std::string *message) { return loadReelFiles(rs, parent, pickAafs(parent), message); };
}

// after the dialog: every reel's media, then one session with all of them (on the first reel)
static bool finishReels(ReelSet &rs, MediaReport &rep, int &merged, std::string &err) {
  rep = MediaReport();
  merged = 0;
  std::vector<Session> sessions;
  std::vector<double> shift;
  bool combine = rs.count() > 1 || rs.firstOff;
  for (int i = 0; i < rs.count(); i++) {
    if (i == 0 && rs.firstOff) continue; // its tab was closed
    MediaReport r;
    int m = 0;
    if (!finishImport(rs.imp(i), rs.opt(i), r, m, err)) return false;
    rep.found += r.found;
    rep.missing += r.missing;
    rep.copied += r.copied;
    rep.converted += r.converted;
    rep.extracted += r.extracted;
    rep.interleaved += r.interleaved;
    rep.missingNames.insert(rep.missingNames.end(), r.missingNames.begin(), r.missingNames.end());
    if (rep.logPath.empty()) rep.logPath = r.logPath;
    merged += m;
    if (combine) {
      sessions.push_back(rs.imp(i).session());
      // relative to the first AAF's start: the project is built with its offset
      shift.push_back(rs.opt(i).tcOffset - rs.opt(0).tcOffset);
    }
  }
  if (combine && !sessions.empty()) {
    // combineReels shifts its first reel too, so the offsets stay right without the first AAF
    rs.first->adoptSession(combineReels(sessions, shift, rs.opt(0).reelsShareTracks));
  }
  return true;
}

// all three steps with a modal dialog (File > Open project: REAPER waits for the result)
static bool runImport(const std::string &aaf, ImportOptions &opt, Importer &imp, MediaReport &rep, int &merged,
                      std::string &err) {
  std::string defaultDest;
  if (!loadForImport(aaf, opt, imp, err, defaultDest)) return false;
  DialogHost host = makeHost();
  host.projectTracks = nullptr; // a new project: no existing tracks to put the AAF on
  ReelSet rs;
  rs.first = &imp;
  rs.firstOpt = &opt;
  attachReels(host, &rs);
  bool accepted = showImportDialog(GetMainHwnd(), imp, opt, host);
  saveSettings(opt, defaultDest);
  saveMediaFolders(opt.searchFolders); // Locate puts its folder first
  if (!accepted) {
    err.clear(); // closed without importing
    return false;
  }
  return finishReels(rs, rep, merged, err);
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
  if (!opt.trackTemplate.empty()) { // stems and routing from the template, the AAF tracks inside them
    std::vector<TemplateTrack> tpl;
    std::string err;
    if (readTrackTemplate(opt.trackTemplate, &tpl, &err))
      chunks = applyTrackTemplate(chunks, tpl, opt.folders ? imp.session().folders : std::vector<Folder>(), base);
    else
      ShowConsoleMsg(("OLAAF: " + err + " The AAF is imported without it.\n").c_str());
  }
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

  MediaReport r2 = rep;
  std::string report = writeReport(imp, opt, rep, merged);
  if (!report.empty()) r2.logPath = report; // the summary points to the report
  ShowMessageBox(summary(imp, r2, merged).c_str(), "OLAAF", 0);
}

// the import into the open project: the window does not block REAPER (the edit cursor can be moved,
// the project scrolled) - the import runs when the window is closed with Build session
struct PendingImport {
  ImportOptions opt;
  Importer imp;
  DialogHost host;
  std::string defaultDest;
  ReelSet reels;
};

static void importIntoProject() {
  if (HWND open = openDialogHwnd()) { // one at a time: the open one comes to the front
    ShowWindow(open, SW_SHOW);
    SetForegroundWindow(open);
    return;
  }
  // the start window first: what OLAAF does, the AAFs opened before, the button to choose one
  applyLook();
  std::vector<std::string> files = showStartWindow(GetMainHwnd(), loadRecent(), [](HWND parent) { return pickAafs(parent, true); });
  if (files.empty()) return;
  const std::string fn = files[0];

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
  addRecent(fn);
  p->reels.first = &p->imp;
  p->reels.firstOpt = &p->opt;
  attachReels(p->host, &p->reels);
  if (files.size() > 1) { // several chosen at once: the others are reels of the same import
    std::string msg;
    loadReelFiles(&p->reels, GetMainHwnd(), std::vector<std::string>(files.begin() + 1, files.end()), &msg);
    if (!msg.empty()) ShowConsoleMsg(("OLAAF: " + msg + "\n").c_str());
  }
  HWND h = openImportDialog(GetMainHwnd(), p->imp, p->opt, p->host, [p](bool accepted) {
    saveSettings(p->opt, p->defaultDest);
    saveMediaFolders(p->opt.searchFolders);
    if (accepted) {
      MediaReport rep;
      int merged = 0;
      std::string e;
      if (finishReels(p->reels, rep, merged, e)) insertTracks(p->imp, p->opt, rep, merged);
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
