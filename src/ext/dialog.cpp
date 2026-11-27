// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The import dialog is drawn by hand (like the other dmajorsound plugins), so it looks the same
// on macOS and Windows: square controls, explanations bottom left, light and dark mode.
//
// Layout ("arrange view"): header with the AAF's frame rate and start, a toolbar with the import
// switches and the timecode offset, the arrange window (folders and tracks with their names in
// the lanes, like REAPER), a bar for the selected event and a bar for media and import.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "dialog.h"
#include "../core/track_template.h"
#include "../core/choices.h"
#include "../core/reels.h"
#include "theme.h"
#include "logo.h"
#include "../core/util.h"
#include "../core/audiofile.h"
#include "../core/peaks.h"
#include "fonts.h"
#include "round.h"
#include "update.h"


#ifndef _WIN32
#include "../../third_party/reaper-sdk/WDL/swell/swell-dlggen.h"
#ifndef SWELL_DLG_WS_DEFAULT_SCALING
#define SWELL_DLG_WS_DEFAULT_SCALING 0
#endif
#define IDD_OLAAF_IMPORT 4712
SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_OLAAF_IMPORT, SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE | SWELL_DLG_WS_OPAQUE | SWELL_DLG_WS_RESIZABLE,
                                   "OLAAF", 1180, 800, 1.0)
BEGIN
END
SWELL_DEFINE_DIALOG_RESOURCE_END(IDD_OLAAF_IMPORT)
#define IDD_OLAAF_START 4713
SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_OLAAF_START, SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE | SWELL_DLG_WS_OPAQUE | SWELL_DLG_WS_RESIZABLE,
                                   "OLAAF", 760, 540, 1.0)
BEGIN
END
SWELL_DEFINE_DIALOG_RESOURCE_END(IDD_OLAAF_START)
#define OL_GETUSER(h) GetWindowLong(h, GWL_USERDATA)
#define OL_SETUSER(h, v) SetWindowLong(h, GWL_USERDATA, (LONG_PTR)(v))
#else
#include <shlobj.h>
#define OL_GETUSER(h) GetWindowLongPtr(h, GWLP_USERDATA)
#define OL_SETUSER(h, v) SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)(v))
#endif
#undef min
#undef max

namespace olaaf {

// native folder chooser (macOS / Linux through SWELL, Windows through the shell)
static bool pickFolder(HWND parent, const char *title, const std::string &start, std::string *out) {
#ifdef _WIN32
  (void)start;
  wchar_t wtitle[256];
  MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
  BROWSEINFOW bi;
  memset(&bi, 0, sizeof(bi));
  bi.hwndOwner = parent;
  bi.lpszTitle = wtitle;
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl) return false;
  wchar_t wpath[MAX_PATH * 4];
  bool ok = SHGetPathFromIDListW(pidl, wpath) != 0;
  CoTaskMemFree(pidl);
  if (!ok) return false;
  char buf[MAX_PATH * 8];
  WideCharToMultiByte(CP_UTF8, 0, wpath, -1, buf, sizeof(buf), NULL, NULL);
  *out = buf;
  return true;
#else
  (void)parent;
  char buf[4096] = {0};
  if (!BrowseForDirectory(title, start.empty() ? NULL : start.c_str(), buf, sizeof(buf)) || !buf[0]) return false;
  *out = buf;
  return true;
#endif
}

static void *g_hInst = nullptr;
void dialogSetInstance(void *h) { g_hInst = h; }
namespace {

const int W = 1180, H = 742;

#define kFace (theme().face)
#define kPanel (theme().panel)
#define kLine (theme().line)
#define kSoft (theme().soft)
#define kMute (theme().mute)
#define kInk (theme().ink)
#define kBlue (theme().blue)
#define kBlueSoft (theme().blueSoft)
#define kRed (theme().red)
const int kPeakTimer = 7, kPlayTimer = 8, kTipTimer = 9, kUpdTimer = 10, kBlinkTimer = 11;
const COLORREF kWhiteFixed = RGB(0xff, 0xff, 0xff);
// text on the blue accent: white in light mode, near-black on the brighter dark-mode blue
#define kWhite (olaaf::darkTheme() ? RGB(0x12, 0x1a, 0x26) : kWhiteFixed)
#define kWarn (olaaf::darkTheme() ? RGB(0xf0, 0xb2, 0x4a) : RGB(0xc7, 0x7c, 0x0e))
#define kPop (olaaf::darkTheme() ? mixColor(kPanel, kInk, 0.05) : kPanel) // light: white like the window
COLORREF mixColor(COLORREF a, COLORREF b, double t);
#define kFadeLine (mixColor(kInk, kPanel, 0.45)) // fade curves: quieter than text
#define kCounterBg kPanel
#define kCounterInk kBlue

const uint32_t kPalette[10] = {0x5580d4, 0x1F9A8A, 0x4E8F3A, 0xB8962E, 0xD9822B,
                               0xC8423B, 0xB0457E, 0x7A5AC8, 0x6B7480, 0x2B3440};

COLORREF rgbToRef(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// ------------------------------------------------------------------ geometry (layout pixels)
// header 0-56, toolbar 56-108, arrange window 108-633, event bar, media bar, footer
const double kL = 16, kR = W - 16;
const double kTopH = 64;                      // the bar on top
const double kAT = kTopH + 12;                // arrange window top
const double kHdr = 220;                      // lane header width
const double kRuler = 30, kRowH = 36, kSb = 12;
const int kRows = 15;
const double kLT = kAT + 1 + kRuler;          // first lane
const double kLB = kLT + kRows * kRowH;       // below the last lane
const double kAB = kLB + kSb + 2;             // arrange window bottom
const double kTlx = kL + kHdr + 1;            // timeline left
const double kTlr = kR - 1 - kSb;             // timeline right (vertical scrollbar beside it)
const double kFootH = 28;                         // footer: hints for what is under the mouse
const double kEvT = kAB + 4, kEvB = H - kFootH - 2; // the bar under the arrange view

enum HitId {
  H_NONE, H_THEME, H_VIDEO, H_MARKERS, H_FOLDERS, H_ZOOM_FIT, H_TRACK_CHECK, H_TRACK_COLOR,
  H_ZOOM_IN, H_ZOOM_OUT, H_MERGE, H_FADES, H_GAIN, H_COPY_YES, H_COPY_NO, H_SUBDIR, H_DEST_CHOOSE, H_LOCATE,
  H_CANCEL, H_IMPORT, H_TC_PLUS, H_TC_MINUS, H_TC_VALUE, H_TC_CURSOR, H_PAL, H_PAL_CUSTOM, H_PAL_NONE,
  H_LANES, H_TRACK_NAME, H_FOLD_ASSIGN, H_FOLD_NONE, H_FOLD_RENAME, H_FOLD_DELETE, H_FOLD_NEW, H_FOLD_POP,
  H_FOLD_COLOR, H_PAL_BG, H_FROW_RENAME, H_FROW_COLOR, H_EV_REMOVE, H_EV_RESTORE, H_EV_UP, H_EV_DOWN,
  H_EV_AUTOMATION, H_EV_RESET, H_EV_MUTE, H_NEW_FOLDER, H_TRACK_FOLDERMENU, H_TRACK_DELETE, H_VIDEO_CHOOSE, H_SHOW_AUTO, H_TRACK_DEST, H_DEST_NEW, H_DEST_MATCHALL, H_DEST_PICK, H_DEST_POP, H_DEST_OVERWRITE,
  H_OPTS, H_OPTS_BG, H_GAIN_ITEMS, H_GAIN_TRACK, H_GAIN_OFF, H_FPS_OK, H_FPS_SHOW, H_EV_PLAY, H_TL_PLAY, H_RULER, H_HELP, H_HELP_CLOSE, H_HELP_BG, H_UPDATE, H_EV_SWAP, H_TRACK_SWAP, H_GONIO_CLOSE, H_GONIO_BG, H_GONIO_TOGGLE, H_TRACK_SOLO, H_SAMPLES, H_MISS_OPEN, H_MISS_BG, H_MISS_PICK, H_THEME_PICK, H_THEME_BG, H_RULES, H_RULES_BG, H_RULE_COLOR, H_RULE_NAME, H_RULE_WORDS, H_RULES_SORT, H_RULES_RESET, H_TPL_CHOOSE, H_TPL_CLEAR, H_RULES_CLOSE, H_FRESH, H_REEL, H_REEL_ADD, H_REEL_REMOVE, H_MISS_PAGE, H_MISS_CLOSE, H_TC_RESET, H_REMOVE_EMPTY, H_TRACK_STEREO, H_FOLD_TOGGLE, H_SORT_NOW, H_REEL_SHARE, H_REEL_OWN, H_REEL_ASK_BG, H_REEL_MODE, H_SORT_RESET, H_EMBED_LOAD, H_EMBED_SKIP, H_EMBED_BG, H_SORT_SUBS, H_HELP_MANUAL,
  H_START_PICK, H_START_RECENT, H_START_CANCEL, H_START_FMT
};

struct Hit {
  RECT r;
  int id;
  int idx;
  const char *tip;
};

struct ClipHit {
  RECT r;
  int id;
};

// one line in the arrange window: a folder, a track, or the "new track" drop line
struct Row {
  int kind;            // 0 track, 1 folder, 2 drop line
  int track = -1;
  std::string folder;
};

// the start window: what it offers, and what was picked
struct StartData {
  std::vector<std::string> recent;                              // AAFs opened before, newest first
  std::function<std::vector<std::string>(HWND parent)> pick;    // the file chooser (several at once)
  std::vector<std::string> result;
};

struct State {
  PeakCache peaks;         // waveform overviews, read in the background
  int peakSeen = -1;       // peaks.version() at the last paint
  Importer *imp = nullptr;
  ImportOptions *opt = nullptr;
  const DialogHost *host = nullptr;
  HWND hwnd = nullptr;
  double scale = 1.0;      // device pixels per layout pixel (DPI x window zoom)
  double base = 1.0;       // DPI scale (Windows), 1 on macOS
  double ox = 0, oy = 0;   // layout origin in the window (centered)
  std::vector<Hit> hits;
  std::vector<ClipHit> clipHits;
  std::vector<Row> rows;   // what the arrange window shows, top to bottom
  std::vector<int> order;  // track indices in the order they are shown
  const char *tip = nullptr;
  int scrollRow = 0;
  double viewStart = 0, viewLen = 60;
  bool tcMinus = false;
  double tcAbs = 0;
  RECT vTrack{}, vThumb{}, hTrack{}, hThumb{}, lanesRc{};
  double rangeA = 0, rangeB = 1;
  int drag = 0;            // 1 vertical thumb, 2 horizontal thumb, 3 events
  int dragFromX = 0, dragFromY = 0;
  double dragValue = 0;
  bool dragMoved = false;
  int dragRows = 0;        // tracks the events are being dragged by (in shown order)
  bool dragBlocked = false; // ... onto a spot another event holds, with no free track beyond
  int dragEff = 0;          // where they really go (the next free track when the spot is taken)
  int dragFromPos = 0;     // shown position of the track the drag started on
  int pressedClip = 0;
  std::set<int> selTracks; // tracks selected by their names (to drag them into a folder)
  int pressedTrack = -1;
  int anchorTrack = -1;    // Shift-click selects from here to the clicked track
  int dropRow = -1;        // slot (line above that row) the selected tracks would be dropped at
  int dragMouseY = 0;
  int rangeFromRow = -1;   // dragging over names selects from this row
  unsigned lastNameClick = 0; // double-click on a track name renames it
  int lastNameTrack = -1;
  RECT nameRc{};
  std::set<int> sel;       // selected clip ids
  bool moveAutomation = true;
  bool edited = false;
  bool merge = true;
  bool accepted = false;
  bool modeless = false;                 // REAPER stays usable while the window is open
  bool optsOpen = false;                 // the Options panel
  struct { double l, t, r, b; } optsBtn{0, 0, 0, 0};
  bool fpsDismissed = false;             // the frame rate warning was closed
  // audio inside the AAF: asked once per AAF whether to unpack it now (waveforms, listening) or show the items only
  // which media files exist (mediaThere)
  Importer *exImp = nullptr;
  int exRev = -1, mediaRev = 0;
  long long exTick = 0;
  std::vector<char> ex;
  StartData *start = nullptr;            // the start window (no AAF yet)
  int startHover = -1;
  bool embedAsk = false;
  std::set<Importer *> embedAsked;
  struct EmbedJob { // one AAF being unpacked on a worker thread
    Importer *imp = nullptr;
    std::string dir;
    std::vector<int> media;
    std::thread th;
    std::mutex mu;
    std::vector<std::pair<int, std::string>> ready; // unpacked, not yet put into the session (the window does that)
    std::atomic<int> done{0};
    std::atomic<bool> stop{false}, finished{false};
  };
  std::unique_ptr<EmbedJob> embedJob;
  std::vector<std::pair<Importer *, std::string>> embedQueue; // AAFs waiting their turn
  bool reelAsk = false;                  // after adding AAFs: same tracks or their own tracks?
  bool reelAskRelated = false;           // ... they look like one film (numbered reels / same track layout)
  std::string reelAskWhy, reelAskNames;
  int hoverRow = -1;                     // row under the mouse (its track icons show)
  int playClip = 0;                      // the event being listened to (0 = none)
  double playPos = -1;                   // seconds into it
  bool playTL = false;                   // listening to the whole timeline
  double playhead = -1;                  // timeline seconds (AAF time); -1 = where the AAF starts
  double playFrom = 0;                   // where timeline playback started (stop goes back there)
  bool helpOpen = false;                 // the help and shortcut window
  bool tipVisible = false;               // the tooltip shows after a short hover
  std::string tipHold;                   // the hovered hint, copied (st->tip points here then)
  std::vector<std::string> stereoTips;   // the L+R button's hint per track
  std::map<long long, int> laneShow;     // reel + track uid (laneKey) -> automation shown: 1 volume (V), 2 pan (P)
  unsigned menuTick = 0;                 // when a right-click menu last opened
  double mouseLX = -1, mouseLY = -1;     // mouse, in layout units
  std::string updTag, updUrl;            // a newer OLAAF on GitHub
  bool blinkOn = true;                   // the update dot blinks
  int updPollsStart = 0;
  int updPolls = 0;
  // the stereo meter while a stereo event plays
  // undo / redo of the edits made here (tracks, folders and the selection as they were)
  struct Snap {
    std::vector<Track> tracks;
    std::vector<Folder> folders;
    std::set<int> sel;
    bool edited;
  };
  std::vector<Snap> undo, redo;
  // drag to select events
  bool marquee = false;
  double mqX0 = 0, mqY0 = 0, mqX1 = 0, mqY1 = 0; // device pixels
  std::set<int> mqBase;                  // the selection before (Shift adds)
  bool showSamples = false;              // positions also in samples
  bool missingOpen = false;              // the list of missing media
  int missScroll = 0;                    // ... scrolled by this many files
  bool rulesOpen = false;                // the folder sorting rules and the track template
  std::set<std::string> closedFolders;   // folders shown closed in the track list (their tracks still import)
  bool choicesBack = false;              // the last import's choices for this AAF were put back
  bool freshNow = false;                 // "Start fresh" was the last step (nothing to go back to)
  struct Fresh { std::vector<Track> tracks; std::vector<Folder> folders; std::vector<VideoClip> video; ImportOptions opt; };
  Fresh fresh;                           // how it was before, for "Start fresh"
  // reels: what each AAF tab had when another one was shown
  struct ReelView {
    bool inited = false;
    int scrollRow = 0;
    double viewStart = 0, viewLen = 60, tcAbs = 0, playhead = -1;
    bool tcMinus = false, edited = false, merge = true, choicesBack = false, freshNow = false;
    std::set<int> sel, solo;
    std::vector<Snap> undo, redo;
    Fresh fresh;
  };
  std::vector<ReelView> reelViews;
  int reel = 0;                          // the AAF shown
  std::string reelMsg;
  std::string tplLoaded;                 // the template the list below is from
  std::vector<olaaf::TemplateTrack> tplTracks;
  bool themeMenu = false;                // the look menu in the footer
  std::vector<std::string> nameTips;     // per track: the full name (for the footer) when it is shortened
  std::string headTip;                   // everything about the AAF, for the footer
  double missingX = 0;
  std::set<int> solo;                   // soloed tracks (uid): only these play; listening only, not the import
  bool gonioOpen = false;               // the stereo meter is wanted (shows while a stereo event is selected)
  PcmReader rdL, rdR;                    // the files the meter reads (one stereo file: both the same)
  int rdChL = 0, rdChR = 1;
  bool rdSwap = false;
  double rdOffset = 0;                   // file time where the event starts
  double corr = 0;                       // smoothed correlation
  std::vector<std::pair<float, float>> gonio; // the latest points (L, R)
  std::function<void(bool)> onClose;     // modeless: called after the window is gone
  // popovers
  int palTrack = -1;       // color popover for this track ...
  std::string palFolder;   // ... or for this folder
  double palX = 0, palY = 0, palY0 = 0;
  int folderTrack = -1;    // folder popover for this track
  int destTrack = -1;      // "put on" popover for this track (an existing REAPER track or a new one)
  double dpX = 0, dpY = 0, dpY0 = 0;
  int destScroll = 0;
  std::vector<std::pair<std::string, std::string>> projTracks; // GUID, name - read when the popover opens
  double fpX = 0, fpY = 0, fpY0 = 0;
  // in-place text editor
  HWND tcEdit = nullptr;
  int editKind = 0;        // 0 none, 1 timecode offset, 2 folder name, 3 audio subfolder, 4 track name
  int editTrack = -1;
  std::string editKey;
  RECT tcRc{}, subRc{}, editRc{};
  HBRUSH editBrush = nullptr;
  HFONT fTheme = nullptr, fCheck = nullptr, fClip = nullptr, fTitle = nullptr, fTitleThin = nullptr, fTag = nullptr,
        fBody = nullptr, fBold = nullptr, fSmall = nullptr, fSec = nullptr, fMono = nullptr, fMonoBig = nullptr,
        fMonoSmall = nullptr, fHead = nullptr, fWord = nullptr, fKey = nullptr, fName = nullptr, fTiny = nullptr, fMicro = nullptr, fHero = nullptr;
};

HFONT makeFont(double px, bool bold, double scale, bool mono = false) {
  const char *face = uiFontFace(mono); // IBM Plex, embedded
  return CreateFont(-(int)std::lround(px * scale), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face);
}

void makeFonts(State *st) {
  double s = st->scale;
  st->fTheme = makeFont(13, false, s);
  st->fCheck = makeFont(12, true, s);
  st->fClip = makeFont(10, false, s);
  st->fTitle = makeFont(18, true, s);
  st->fTitleThin = makeFont(18, false, s);
  st->fTag = makeFont(10.5, false, s);
  st->fBody = makeFont(12.5, false, s);
  st->fBold = makeFont(13, true, s);
  st->fSmall = makeFont(11, false, s);
  st->fSec = makeFont(11, false, s); // section labels: plain, not spaced capitals
  st->fMono = makeFont(14, true, s, true);
  st->fMonoBig = makeFont(21, false, s, true);
  st->fMonoSmall = makeFont(10.5, false, s, true);
  st->fHead = makeFont(15, true, s);
  st->fWord = makeFont(20, true, s);
  st->fHero = makeFont(28, true, s);
  st->fKey = makeFont(11.5, true, s, true);
  st->fName = makeFont(11.5, false, s);
  st->fTiny = makeFont(9.5, false, s);
  st->fMicro = makeFont(8.5, false, s);
}

void freeFonts(State *st) {
  HFONT *fs[] = {&st->fTheme, &st->fCheck, &st->fClip, &st->fTitle, &st->fTitleThin, &st->fTag, &st->fBody,
                 &st->fBold, &st->fSmall, &st->fSec, &st->fMono, &st->fMonoBig, &st->fMonoSmall, &st->fHead, &st->fWord, &st->fKey, &st->fName, &st->fTiny, &st->fMicro, &st->fHero};
  for (HFONT *f : fs) {
    if (*f) DeleteObject(*f);
    *f = nullptr;
  }
}

// The layout is drawn at a fixed size (W x H) and zoomed to the window: everything, type included,
// grows with the window. The zoomed layout is centered in the window.
#ifdef _WIN32
// DPI of the monitor the window is on (Windows 10 1607+), else the system DPI
static UINT windowDpi(HWND h) {
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  static GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
  UINT d = fn ? fn(h) : 0;
  if (!d) {
    HDC dc = GetDC(h);
    d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(h, dc);
  }
  return d ? d : 96;
}
#endif

void relayout(State *st) {
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  double zx = cr.right / (W * st->base), zy = cr.bottom / (H * st->base);
  double zoom = std::max(0.5, std::min(zx, zy));
  double scale = st->base * zoom;
  st->ox = std::max(0.0, std::floor((cr.right - W * scale) / 2));
  st->oy = std::max(0.0, std::floor((cr.bottom - H * scale) / 2));
  if (std::fabs(scale - st->scale) > 1e-6 || !st->fBody) {
    st->scale = scale;
    freeFonts(st);
    makeFonts(st);
    if (st->tcEdit) SendMessage(st->tcEdit, WM_SETFONT, (WPARAM)st->fBody, 0);
  }
}

static const double kPi = 3.14159265358979323846;

struct Gfx {
  HDC dc;
  double s;
  double ox = 0, oy = 0;
  int S(double v) const { return (int)std::lround(v * s); } // a length
  int X(double v) const { return (int)std::lround(v * s + ox); }
  int Y(double v) const { return (int)std::lround(v * s + oy); }
  RECT R(double l, double t, double r, double b) const { return {(LONG)X(l), (LONG)Y(t), (LONG)X(r), (LONG)Y(b)}; }
  void fill(double l, double t, double r, double b, COLORREF c) const {
    RECT rc = R(l, t, r, b);
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
  }
  void box(double l, double t, double r, double b, COLORREF line, COLORREF in) const {
    fill(l, t, r, b, line);
    RECT rc = {X(l) + 1, Y(t) + 1, X(r) - 1, Y(b) - 1};
    HBRUSH br = CreateSolidBrush(in);
    FillRect(dc, &rc, br);
    DeleteObject(br);
  }
  void text(HFONT f, COLORREF c, const std::string &str, double l, double t, double r, double b, int align) const {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT rc = R(l, t, r, b);
    olDrawText(dc, str.c_str(), &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
  }
  // text placed by its baseline (layout y), for type that must line up with a graphic
  void textBase(HFONT f, COLORREF c, const std::string &str, double x, double baseline) const {
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRIC tm;
    memset(&tm, 0, sizeof(tm));
    GetTextMetrics(dc, &tm);
    int asc = tm.tmAscent > 0 ? tm.tmAscent : S(12);
    int hgt = tm.tmHeight > 0 ? tm.tmHeight : asc + asc / 4;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT rc = {X(x), Y(baseline) - asc, X(x) + S(600), Y(baseline) - asc + hgt};
    olDrawText(dc, str.c_str(), &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
  }
  double width(HFONT f, const std::string &str) const {
    HGDIOBJ old = SelectObject(dc, f);
    RECT rc = {0, 0, 4000, 100};
    olDrawText(dc, str.c_str(), &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return (rc.right - rc.left) / s;
  }
  void line(double x1, double y1, double x2, double y2, COLORREF c, double width = 1) const {
    HPEN p = CreatePen(PS_SOLID, std::max(1, S(width)), c);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, X(x1), Y(y1), NULL);
    LineTo(dc, X(x2), Y(y2));
    SelectObject(dc, old);
    DeleteObject(p);
  }
  // rounded rectangles (smooth on Windows and macOS)
  void rfill(double l, double t, double r, double b, double rad, COLORREF c) const {
    olRoundFill(dc, l * s + ox, t * s + oy, r * s + ox, b * s + oy, rad * s, c);
  }
  void rbox(double l, double t, double r, double b, double rad, COLORREF line, COLORREF in) const {
    rfill(l, t, r, b, rad, line);
    double i = 1.0 / s;
    rfill(l + i, t + i, r - i, b - i, std::max(0.0, rad - i), in);
  }
  // a smooth (anti-aliased, sub-pixel) polygon
  void polyAA(const std::vector<std::pair<double, double>> &pts, COLORREF c) const {
    std::vector<double> xy;
    for (auto &q : pts) {
      xy.push_back(q.first * s + ox);
      xy.push_back(q.second * s + oy);
    }
    olPolyFill(dc, xy.data(), (int)pts.size(), c);
  }
  void poly(const std::vector<std::pair<double, double>> &pts, COLORREF c) const {
    std::vector<POINT> p;
    for (auto &q : pts) p.push_back({(LONG)X(q.first), (LONG)Y(q.second)});
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    Polygon(dc, p.data(), (int)p.size());
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
  }
  void disc(double cx, double cy, double r, COLORREF c) const {
    std::vector<std::pair<double, double>> p;
    for (int i = 0; i < 24; i++) p.push_back({cx + r * std::cos(i * kPi / 12), cy + r * std::sin(i * kPi / 12)});
    poly(p, c);
  }
  // The few symbols IBM Plex has no glyph for, drawn so they look the same everywhere.
  // Returns false for anything else (then it is plain text).
  bool icon(const std::string &glyph, double cx, double cy, COLORREF c, COLORREF bg) const {
    if (glyph == "\xe2\x96\xbe") { // small caret down
      poly({{cx - 4, cy - 2}, {cx + 4, cy - 2}, {cx, cy + 2.5}}, c);
    } else if (glyph == "\xe2\x96\xbc") { // down
      poly({{cx - 5, cy - 3}, {cx + 5, cy - 3}, {cx, cy + 4}}, c);
    } else if (glyph == "\xe2\x96\xb6") { // play
      poly({{cx - 4, cy - 6}, {cx + 6, cy}, {cx - 4, cy + 6}}, c);
    } else if (glyph == "\xe2\x96\xa0") { // stop
      fill(cx - 5, cy - 5, cx + 5, cy + 5, c);
    } else if (glyph == "\xe2\x96\xb2") { // up
      poly({{cx - 5, cy + 3}, {cx + 5, cy + 3}, {cx, cy - 4}}, c);
    } else if (glyph == "\xe2\x9c\x8e") { // pencil: body, then the tip
      poly({{cx - 2.6, cy + 4.6}, {cx + 3.6, cy - 1.6}, {cx + 5.6, cy + 0.4}, {cx - 0.6, cy + 6.6}}, c);
      poly({{cx + 4.4, cy - 2.8}, {cx + 5.6, cy - 4}, {cx + 7.6, cy - 2}, {cx + 6.4, cy - 0.8}}, c);
      poly({{cx - 2.6, cy + 4.6}, {cx - 0.6, cy + 6.6}, {cx - 4, cy + 8}}, c);
    } else if (glyph == "\xe2\x87\xa5") { // arrow to a bar: put on a track
      line(cx - 5, cy, cx + 2, cy, c, 1.5);
      poly({{cx + 0.5, cy - 3.5}, {cx + 4, cy}, {cx + 0.5, cy + 3.5}}, c);
      line(cx + 5.5, cy - 4.5, cx + 5.5, cy + 4.5, c, 1.5);
    } else if (glyph == "\xe2\x98\x80") { // sun
      disc(cx, cy, 3.2, c);
      for (int i = 0; i < 8; i++) {
        double a = i * kPi / 4;
        line(cx + 5 * std::cos(a), cy + 5 * std::sin(a), cx + 7 * std::cos(a), cy + 7 * std::sin(a), c, 1.2);
      }
    } else if (glyph == "\xe2\x98\xbe") { // moon: a disc with a disc of the background cut out
      disc(cx, cy, 6, c);
      disc(cx + 3.2, cy - 2.2, 5, bg);
    } else
      return false;
    return true;
  }
  // a check mark drawn as two strokes, centred in a 16 x 16 box at (x, y)
  // the check mark of a 16 x 16 box at x, y: two filled strokes drawn the same way as the box
  // (smooth fill), so it sits in the middle on every system (lines were rounded differently)
  void check(double x, double y, COLORREF c) const {
    const double cx = x + 8, cy = y + 8, w = 1.05; // half the stroke width
    auto stroke = [&](double ax, double ay, double bx, double by) {
      double dx = bx - ax, dy = by - ay, l = std::sqrt(dx * dx + dy * dy);
      double nx = -dy / l * w, ny = dx / l * w, ex = dx / l * w * 0.5, ey = dy / l * w * 0.5;
      polyAA({{cx + ax + nx - ex, cy + ay + ny - ey}, {cx + bx + nx + ex, cy + by + ny + ey},
              {cx + bx - nx + ex, cy + by - ny + ey}, {cx + ax - nx - ex, cy + ay - ny - ey}}, c);
    };
    stroke(-4.0, 0.2, -1.2, 3.0); // the short stroke
    stroke(-1.2, 3.0, 4.2, -3.4); // the long one
  }
};

RECT devRect(State *st, double l, double t, double r, double b) {
  return {(LONG)std::lround(l * st->scale + st->ox), (LONG)std::lround(t * st->scale + st->oy),
          (LONG)std::lround(r * st->scale + st->ox), (LONG)std::lround(b * st->scale + st->oy)};
}

void addHit(State *st, double l, double t, double r, double b, int id, int idx, const char *tip) {
  Hit h;
  h.r = devRect(st, l, t, r, b);
  h.id = id;
  h.idx = idx;
  h.tip = tip;
  st->hits.push_back(h);
}

void checkbox(State *st, const Gfx &g, double x, double y, bool on, int id, int idx, const char *tip) {
  if (on) {
    g.rfill(x, y, x + 16, y + 16, 4, kBlue);
    g.check(x, y, kWhite);
  } else
    g.rbox(x, y, x + 16, y + 16, 4, theme().empty, kPanel);
  addHit(st, x - 4, y - 4, x + 20, y + 20, id, idx, tip);
}

void button(State *st, const Gfx &g, double l, double t, double r, double b, const std::string &label, int id,
            int idx, const char *tip, bool primary = false, bool enabled = true, bool active = false) {
  double rad = std::min(8.0, (b - t) / 2);
  if (primary) {
    g.rfill(l, t, r, b, rad, enabled ? kBlue : kLine);
    g.text(st->fBold, enabled ? kWhite : kMute, label, l, t, r, b, DT_CENTER);
  } else {
    g.rbox(l, t, r, b, rad, active ? kBlue : kLine, active ? kBlueSoft : kPanel);
    if (!g.icon(label, (l + r) / 2, (t + b) / 2, enabled ? kInk : kMute, kPanel))
      g.text(st->fBody, enabled ? kInk : kMute, label, l, t, r, b, DT_CENTER);
  }
  if (enabled) addHit(st, l, t, r, b, id, idx, tip);
}

double niceStep(double secPerPx) {
  static const double steps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
  for (double s : steps)
    if (s / secPerPx >= 110) return s;
  return 7200;
}

double tcOffset(const State *st) { return st->tcMinus ? -st->tcAbs : st->tcAbs; }

// automation shown per track: each reel (tab) has its own track uids
long long laneKey(const State *st, int uid) { return ((long long)st->reel << 32) | (unsigned)uid; }

int reelCount(const State *st) { return st->host && st->host->reelCount ? std::max(1, st->host->reelCount()) : 1; }

bool reelHidden(const State *st, int i) { return st->host && st->host->reelHidden && st->host->reelHidden(i); }

// the AAFs with a tab (a closed first tab stays loaded but is not shown or imported)
std::vector<int> shownReels(const State *st) {
  std::vector<int> v;
  for (int i = 0; i < reelCount(st); i++)
    if (!reelHidden(st, i)) v.push_back(i);
  return v;
}

bool reelAt(const State *st, int i, Importer **imp, ImportOptions **opt) {
  if (i == st->reel) {
    *imp = st->imp;
    *opt = st->opt;
    return true;
  }
  return st->host && st->host->reelAt && st->host->reelAt(i, imp, opt);
}

// where reel i lands in REAPER (the shown one: with the offset being typed)
bool reelSpan(const State *st, int i, double *a, double *b) {
  Importer *imp;
  ImportOptions *opt;
  if (!reelAt(st, i, &imp, &opt)) return false;
  const Session &s = imp->session();
  double off = i == st->reel ? tcOffset(st) : opt->tcOffset;
  *a = s.firstClip() + off;
  *b = std::max(s.end, s.firstClip()) + off;
  return true;
}

int reelOverlaps(const State *st, int i) { // the first reel it lands on top of, or -1
  double a0, a1, b0, b1;
  if (!reelSpan(st, i, &a0, &a1)) return -1;
  for (int k = 0; k < reelCount(st); k++)
    if (k != i && !reelHidden(st, k) && reelSpan(st, k, &b0, &b1) && reelsOverlap(a0, a1, b0, b1)) return k;
  return -1;
}


void sessionRange(const Session &s, double *a, double *b) {
  bool any = false;
  *a = s.start;
  *b = s.start + 60;
  for (auto &t : s.tracks)
    for (auto &c : t.clips) {
      if (!any || c.pos < *a) *a = c.pos;
      if (!any || c.pos + c.len > *b) *b = c.pos + c.len;
      any = true;
    }
  if (*b - *a < 1) *b = *a + 1;
}

std::string dbText(double gain) {
  if (gain <= 0.0) return "-inf dB";
  double db = 20.0 * std::log10(gain);
  if (std::fabs(db) < 0.05) return "0 dB";
  return strprintf("%+.1f dB", db);
}

std::string fpsText(const Session &s) {
  if (std::fabs(s.fps - std::round(s.fps)) < 0.001) return strprintf("%d fps%s", (int)std::lround(s.fps), s.dropFrame ? " DF" : "");
  return strprintf("%.3g fps%s", s.fps, s.dropFrame ? " DF" : "");
}

std::string shortPath(std::string p, size_t maxLen) {
#ifndef _WIN32
  if (const char *home = getenv("HOME")) {
    std::string h = home;
    if (!h.empty() && p.compare(0, h.size(), h) == 0) p = "~" + p.substr(h.size());
  }
#endif
  if (p.size() <= maxLen) return p;
  size_t cut = p.size() - (maxLen - 1);
  while (cut < p.size() && ((unsigned char)p[cut] & 0xC0) == 0x80) cut++; // keep UTF-8 characters whole
  return "\xe2\x80\xa6" + p.substr(cut);
}

// the first n bytes of s, not splitting a UTF-8 character
std::string utf8Head(const std::string &s, size_t n) {
  if (n >= s.size()) return s;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return s.substr(0, n);
}

// ------------------------------------------------------------------ rows

void buildRows(State *st) {
  Session &s = st->imp->session();
  st->rows.clear();
  st->order.clear();
  std::vector<bool> placed(s.tracks.size(), false);
  if (st->opt->folders && !s.folders.empty()) {
    // a folder's row and its tracks; hidden: inside a closed folder (its tracks keep their place, without a row)
    auto add = [&](const Folder &f, bool hidden) {
      if (!hidden) st->rows.push_back({1, -1, f.key});
      bool closed = hidden || st->closedFolders.count(f.key) > 0;
      for (size_t i = 0; i < s.tracks.size(); i++)
        if (s.tracks[i].group == f.key) {
          if (!closed) st->rows.push_back({0, (int)i, std::string()});
          st->order.push_back((int)i);
          placed[i] = true;
        }
      return closed;
    };
    for (auto &f : s.folders) {
      if (!f.parent.empty() && s.folder(f.parent)) continue; // comes with the folder around it
      bool closed = add(f, false);
      for (auto &sub : s.folders)
        if (sub.parent == f.key) add(sub, closed);
    }
  }
  for (size_t i = 0; i < s.tracks.size(); i++)
    if (!placed[i]) {
      st->rows.push_back({0, (int)i, std::string()});
      st->order.push_back((int)i);
    }
  st->rows.push_back({2, -1, std::string()});
}

int posOfTrack(const State *st, int track) {
  for (size_t i = 0; i < st->order.size(); i++)
    if (st->order[i] == track) return (int)i;
  return -1;
}

// shown track position at row index ri (a folder row counts as the track below it; past the end = new tracks).
// Tracks in a closed folder keep their positions but have no row.
int posAtRow(const State *st, int ri) {
  if (ri < 0) ri = 0;
  for (int i = ri; i < (int)st->rows.size(); i++)
    if (st->rows[(size_t)i].kind == 0) return posOfTrack(st, st->rows[(size_t)i].track);
  int n = (int)st->order.size();
  return ri >= (int)st->rows.size() ? n + ri - (int)st->rows.size() + 1 : n;
}

// row index that shows shown track position pos (new tracks go on and below the drop line); a track in a
// closed folder: the folder's row
int rowOfPos(const State *st, int pos) {
  if (pos >= 0 && pos < (int)st->order.size()) {
    int t = st->order[(size_t)pos];
    for (size_t i = 0; i < st->rows.size(); i++)
      if (st->rows[i].kind == 0 && st->rows[i].track == t) return (int)i;
    Session &s = st->imp->session();
    std::string grp = s.tracks[(size_t)t].group;
    for (int up = 0; up < 2; up++) {
      for (size_t i = 0; i < st->rows.size(); i++)
        if (st->rows[i].kind == 1 && st->rows[i].folder == grp) return (int)i;
      const Folder *f = s.folder(grp); // its sub-folder has no row: the folder around it
      if (!f) break;
      grp = f->parent;
    }
  }
  return (int)st->rows.size() - 1 + (pos - (int)st->order.size());
}

// a closed folder opens when one of its tracks is needed (events moved there, Tab)
void openFolderOf(State *st, int track) {
  Session &s = st->imp->session();
  if (track < 0 || track >= (int)s.tracks.size()) return;
  const std::string &grp = s.tracks[(size_t)track].group;
  const Folder *f = s.folder(grp);
  bool opened = st->closedFolders.erase(grp) > 0;
  if (f && !f->parent.empty()) opened = st->closedFolders.erase(f->parent) > 0 || opened;
  if (opened) buildRows(st);
}

int folderIndex(const Session &s, const std::string &key) {
  for (size_t i = 0; i < s.folders.size(); i++)
    if (s.folders[i].key == key) return (int)i;
  return -1;
}

// the folder a track dropped on row ri goes into ("" = outside any folder)
[[maybe_unused]] std::string folderOfRow(const State *st, int ri) {
  if (ri < 0) return std::string();
  if (ri >= (int)st->rows.size()) return std::string();
  const Row &r = st->rows[(size_t)ri];
  if (r.kind == 1) return r.folder;
  if (r.kind == 0) return st->imp->session().tracks[(size_t)r.track].group;
  return std::string();
}

// where moved tracks land when dropped at slot (the line above row `slot`; past the drop line = outside)
std::string dropKey(const State *st, int slot) {
  int dropIdx = (int)st->rows.size() - 1; // the "new track" line
  if (slot < 0) return std::string();
  if (slot > dropIdx) return std::string();
  int above = slot - 1;
  if (above < 0) return st->rows.empty() || st->rows[0].kind != 1 ? std::string() : st->rows[0].folder;
  const Row &r = st->rows[(size_t)above];
  if (r.kind == 1) return r.folder;
  if (r.kind == 0) return st->imp->session().tracks[(size_t)r.track].group;
  return std::string();
}

// boundary between rows under device y (0 = above the first row)
int slotAtY(const State *st, int y) {
  double ly = (y - st->oy) / st->scale;
  int slot = st->scrollRow + (int)std::lround((ly - kLT) / kRowH);
  return std::max(0, std::min(slot, (int)st->rows.size()));
}

int rowAtY(const State *st, int y) {
  double ly = (y - st->oy) / st->scale;
  if (ly < kLT || ly >= kLB) return -1;
  int ri = st->scrollRow + (int)std::floor((ly - kLT) / kRowH);
  return std::min(ri, (int)st->rows.size() - 1);
}

// ------------------------------------------------------------------ painting

// Splits text into at most maxLines lines that fit `width` (at spaces, _ - . or anywhere if needed);
// *cut is set when the last line had to be shortened.
std::vector<std::string> fitLines(const Gfx &g, HFONT f, const std::string &text, double width, int maxLines, bool *cut) {
  std::vector<std::string> lines;
  if (cut) *cut = false;
  size_t i = 0;
  while (i < text.size() && (int)lines.size() < maxLines) {
    // longest prefix (whole UTF-8 characters) that fits
    size_t best = i, j = i;
    while (j < text.size()) {
      size_t k = j + 1;
      while (k < text.size() && ((unsigned char)text[k] & 0xC0) == 0x80) k++;
      if (g.width(f, text.substr(i, k - i)) > width) break;
      best = j = k;
    }
    if (best == i) best = i + 1; // at least one character
    if (best < text.size() && (int)lines.size() + 1 < maxLines) { // break after a separator if there is one
      for (size_t k = best; k > i + 3; k--)
        if (strchr(" _-.", text[k - 1])) {
          best = k;
          break;
        }
    }
    if ((int)lines.size() + 1 == maxLines && best < text.size()) { // the last line: shorten with an ellipsis
      std::string l = text.substr(i, best - i);
      while (!l.empty() && g.width(f, l + "\xe2\x80\xa6") > width) {
        l.pop_back();
        while (!l.empty() && ((unsigned char)l.back() & 0xC0) == 0x80) l.pop_back();
      }
      lines.push_back(l + "\xe2\x80\xa6");
      if (cut) *cut = true;
      return lines;
    }
    lines.push_back(text.substr(i, best - i));
    i = best;
    while (i < text.size() && text[i] == ' ') i++;
  }
  return lines;
}

// the AAF's frame rate against the REAPER project's (0 when unknown or equal)
bool fpsMismatch(State *st, double *projFps = nullptr) {
  if (!st->host || !st->host->projectFps) return false;
  const Session &s = st->imp->session();
  bool drop = false;
  double pf = st->host->projectFps(&drop);
  if (projFps) *projFps = pf;
  if (pf <= 0 || s.fps <= 0) return false;
  return std::fabs(pf - s.fps) > 0.01 || (drop != s.dropFrame && std::fabs(s.fps - 29.97) < 0.02);
}

// the AAF's sample rate against the REAPER project's
bool srMismatch(State *st, double *projSr = nullptr) {
  if (!st->host || !st->host->projectSrate) return false;
  double pr = st->host->projectSrate();
  if (projSr) *projSr = pr;
  const Session &s = st->imp->session();
  return pr > 0 && s.sampleRate > 0 && std::fabs(pr - s.sampleRate) > 1;
}

std::string fpsShort(double f) {
  if (std::fabs(f - std::lround(f)) < 0.001) return strprintf("%d", (int)std::lround(f));
  return strprintf("%.3f", f);
}

// Is a media file there? Asked for every file on every repaint, so the answers are kept for two seconds
// (a drive that was just plugged in still shows up) and until the session or a path changes (mediaRev).
bool mediaThere(State *st, int mi) {
  const Session &s = st->imp->session();
  long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (st->exImp != st->imp || st->exRev != st->mediaRev || st->ex.size() != s.media.size() || now - st->exTick > 2000) {
    st->ex.assign(s.media.size(), 0);
    for (size_t i = 0; i < s.media.size(); i++)
      st->ex[i] = s.media[i].kind == MediaKind::Embedded || fileExists(s.media[i].foundPath);
    st->exImp = st->imp;
    st->exRev = st->mediaRev;
    st->exTick = now;
  }
  return mi >= 0 && mi < (int)st->ex.size() && st->ex[(size_t)mi];
}

// the media the import needs: those of the events that go along
std::set<int> usedMedia(State *st) {
  std::set<int> used;
  for (auto &tr : st->imp->session().tracks)
    if (tr.import)
      for (auto &c : tr.clips)
        if (!c.excluded)
          for (auto &src : c.sources)
            if (src.media >= 0) used.insert(src.media);
  return used;
}

void mediaCount(State *st, int *ok, int *total) {
  *ok = *total = 0;
  for (int mi : usedMedia(st)) {
    (*total)++;
    if (mediaThere(st, mi)) (*ok)++;
  }
}

int tracksToImport(State *st) {
  int ntr = 0;
  for (auto &tr : st->imp->session().tracks) {
    if (!tr.import) continue;
    bool any = !tr.created;
    for (auto &c : tr.clips) any = any || !c.excluded;
    ntr += any;
  }
  return ntr;
}

// All open AAFs together: tracks with the same name (and the same "put on" target) share one REAPER
// track, so they count once. `tip` lists the AAFs.
int totalTracks(State *st, std::string *tip) {
  std::set<std::string> keys;
  std::string list;
  for (int i : shownReels(st)) {
    Importer *ri;
    ImportOptions *ro;
    if (!reelAt(st, i, &ri, &ro)) continue;
    int own = 0;
    for (auto &tr : ri->session().tracks) {
      if (!tr.import) continue;
      bool any = !tr.created;
      for (auto &c : tr.clips) any = any || !c.excluded;
      if (!any) continue;
      own++;
      std::string k = lower(tr.name);
      while (!k.empty() && k.back() == ' ') k.pop_back();
      if (!ro->reelsShareTracks) k += strprintf("|reel%d", i); // every AAF its own tracks
      keys.insert(k + "|" + tr.target);
    }
    list += (list.empty() ? "" : ", ") + reelLabel(ri->session(), i) + strprintf(": %d", own);
  }
  if (tip)
    *tip = strprintf("%d tracks in REAPER from all AAFs (%s). %s", (int)keys.size(), list.c_str(),
                     st->opt->reelsShareTracks ? "Tracks with the same name share one track."
                                               : "Every AAF gets its own tracks.");
  return (int)keys.size();
}

// One bar on top: the AAF, where it lands in REAPER, options, import.
// the OLAAF mark, the name and the version, at the top left, centered on `mid`
std::string updateNote(State *st) {
  std::string v = st->updTag;
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
  return v + " is out";
}

// A newer OLAAF (in the footer, as in every dmajorsound tool): a light red dot that blinks and "0.8.1 is out",
// right-aligned to `right`; a click opens the download page. Returns its left edge (`right` when there is none).
double paintUpdateNote(State *st, const Gfx &g, double right, double t, double b) {
  if (st->updTag.empty()) return right;
  std::string note = updateNote(st);
  double tw = g.width(st->fSmall, note), cy = (t + b) / 2, x = right - tw - 13;
  COLORREF dot = RGB(0xF0, 0x6E, 0x6E);
  if (st->blinkOn) g.rfill(x, cy - 3.5, x + 7, cy + 3.5, 3.5, dot);
  else g.rbox(x, cy - 3.5, x + 7, cy + 3.5, 3.5, dot, kFace);
  g.text(st->fSmall, kRed, note, x + 13, t + 1, right, b, DT_LEFT);
  static std::string tip;
  tip = "OLAAF " + st->updTag + " is out. Click to open the download page.";
  addHit(st, x - 4, t + 4, right + 4, b - 4, H_UPDATE, 0, tip.c_str());
  return x;
}

// the brand's width: the name, or the version line with "0.8.1 is out" when there is a newer OLAAF
double brandRight(State *st, const Gfx &g) {
  (void)st;
  return 56 + std::max(60.0, std::min(90.0, g.width(st->fWord, "OLAAF")));
}

void paintBrand(State *st, const Gfx &g, double mid) {
  // dark tile like the Ahh! and Alfred icons, in light and dark mode alike
  const COLORREF tile = RGB(0x20, 0x21, 0x25);
  g.rbox(16, mid - 15, 46, mid + 15, 7, RGB(0x3A, 0x3B, 0x41), tile);
  { // the mark: two events of an edit and a paper plane landing (drawn on a 120 grid, 30 units here)
    auto clip = [&](double x, double y, double w, double h, double fi, double fo) {
      const double k = 0.25, ox0 = 16, oy0 = mid - 15;
      g.polyAA({{ox0 + x * k, oy0 + (y + h) * k}, {ox0 + (x + fi) * k, oy0 + y * k}, {ox0 + (x + w - fo) * k, oy0 + y * k},
                {ox0 + (x + w) * k, oy0 + (y + h) * k}},
               kWhiteFixed);
    };
    // a paper plane coming in to land on the tracks (the AAFUGO mark has one taking off)
    auto pt = [&](double x, double y) { return std::make_pair(16 + x * 0.25, mid - 15 + y * 0.25); };
    // the touchdown point: a blue ring under the plane's nose and a mint dot on it, as on the Ahh! icon
    auto [tx, ty] = pt(80, 46);
    g.rfill(tx - 2.7, ty - 2.7, tx + 2.7, ty + 2.7, 2.7, RGB(0x63, 0x8F, 0xBE));
    g.rfill(tx - 1.8, ty - 1.8, tx + 1.8, ty + 1.8, 1.8, tile);
    g.polyAA({pt(80, 46), pt(32, 22), pt(54, 24)}, kWhiteFixed);
    g.polyAA({pt(80, 46), pt(54, 24), pt(53, 9)}, RGB(0xC9, 0xCC, 0xD3));
    clip(46, 52, 52, 16, 12, 12);
    clip(22, 74, 40, 16, 0, 10);
    g.rfill(tx - 1.1, ty - 1.1, tx + 1.1, ty + 1.1, 1.1, RGB(0x9D, 0xEB, 0xC0)); // touchdown: the mint dot
  }
  g.textBase(st->fWord, kInk, "OLAAF", 56, mid - 15 + 20 * 0.698);
  g.textBase(st->fTiny, kMute, "V " OLAAF_VERSION_STRING " beta", 57, mid + 15);
}

void paintTopBar(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  g.fill(0, 0, W, kTopH, kFace);
  g.fill(0, kTopH - 1, W, kTopH, kLine);
  double mid = kTopH / 2;
  // the OLAAF mark and name: "OLAAF" from the top of the mark, the version on its bottom line
  // (IBM Plex: capitals are 0.698 of the type size)
  double wordR = brandRight(st, g);
  if (st->themeMenu) g.rbox(10, mid - 21, wordR + 6, mid + 21, 8, kBlue, kBlueSoft); // the menu is open
  addHit(st, 10, mid - 21, wordR + 6, mid + 21, H_THEME, 0,
         "OLAAF " OLAAF_VERSION_STRING " beta, developed with AI assistance. dmajorsound.com");
  paintBrand(st, g, mid); // (its update note is clicked on its own)
  g.fill(wordR + 14, mid - 16, wordR + 15, mid + 16, kLine);

  // right side, right to left: Import, Cancel, Options, | Edit cursor, where it lands
  double r = kR;
  int ntr = shownReels(st).size() > 1 ? totalTracks(st, nullptr) : tracksToImport(st);
  std::string imp = strprintf("Import %d track%s", ntr, ntr == 1 ? "" : "s");
  double iw = std::max(120.0, g.width(st->fBold, imp) + 36);
  button(st, g, r - iw, mid - 16, r, mid + 16, imp, H_IMPORT, 0, "Build the session in REAPER (Return).", true, ntr > 0);
  r -= iw + 6;
  double cw = std::max(60.0, g.width(st->fBody, "Cancel") + 24);
  g.text(st->fBody, kMute, "Cancel", r - cw, mid - 16, r, mid + 16, DT_CENTER);
  addHit(st, r - cw, mid - 16, r, mid + 16, H_CANCEL, 0, "Close without importing (Esc).");
  r -= cw + 6;
  button(st, g, r - 30, mid - 15, r, mid + 15, "?", H_HELP, 0, "Help and keyboard shortcuts (H).", false, true, st->helpOpen);
  r -= 36;
  double ow = std::max(90.0, g.width(st->fBody, "Options") + 40);
  st->optsBtn = {r - ow, mid - 15, r, mid + 15};
  button(st, g, r - ow, mid - 15, r, mid + 15, "", H_OPTS, 0, "Fades, stereo, markers, folders, clip gain, video and where the audio goes.",
         false, true, st->optsOpen);
  g.text(st->fBody, kInk, "Options", r - ow + 12, mid - 15, r - 22, mid + 15, DT_LEFT);
  g.icon("\xe2\x96\xbe", r - 14, mid + 0.5, kInk, kPanel);
  r -= ow + 14;
  g.fill(r, mid - 15, r + 1, mid + 15, kLine);
  r -= 14;
  double ew = std::max(90.0, g.width(st->fBody, "Edit cursor") + 24);
  button(st, g, r - ew, mid - 15, r, mid + 15, "Edit cursor", H_TC_CURSOR, 0,
         "The AAF starts at REAPER's edit cursor.");
  r -= ew + 12;
  // where the AAF starts in REAPER: one timecode, click to type
  double tw = 168;
  std::string land = formatTC(s.firstClip() + tcOffset(st), s.fps);
  g.text(st->fSmall, kMute, "Starts in REAPER at", r - tw, mid - 22, r, mid - 6, DT_RIGHT);
  if (st->editKind != 1) g.text(st->fMonoBig, kBlue, land, r - tw, mid - 7, r, mid + 18, DT_RIGHT);
  for (double dx = r - 3; dx > r - tw + 30; dx -= 6) g.fill(dx - 3, mid + 19, dx, mid + 20, theme().empty);
  st->tcRc = devRect(st, r - tw + 20, mid - 6, r, mid + 18);
  static std::string tcTip;
  tcTip = "Where the AAF lands in REAPER. In the AAF it starts at " + formatTC(s.firstClip(), s.fps) +
          ". Click and type HH:MM:SS:FF or just the digits; Return applies, Esc cancels.";
  addHit(st, r - tw, mid - 22, r, mid + 21, H_TC_VALUE, 0, tcTip.c_str());
  double infoR = r - tw - 16;
  if (std::fabs(tcOffset(st)) > 1e-9) { // moved (typed or edit cursor): one click back to the AAF's own timecode
    static std::string rTip;
    rTip = "Back to the AAF's own timecode: it starts at " + formatTC(s.firstClip(), s.fps) + ".";
    double rw = g.width(st->fBody, "Reset TC Offset") + 24; // not just "Reset": the event bar has one too
    double used = std::max(g.width(st->fMonoBig, land), g.width(st->fSmall, "Starts in REAPER at")) + 12;
    double bR = r - std::min(tw, used);   // right next to the timecode
    button(st, g, bR - rw, mid - 13, bR, mid + 13, "Reset TC Offset", H_TC_RESET, 0, rTip.c_str());
    infoR = std::min(infoR, bR - rw - 16);
  }

  // left: the AAF and what it is - it always fits (smaller type, fewer details); all of it is in the footer on hover
  double x0 = wordR + 30, avail = infoR - x0;
  std::string name = baseName(s.aafPath);
  double projFps = 0, projSr = 0;
  bool fpsBad = fpsMismatch(st, &projFps), srBad = srMismatch(st, &projSr);
  int ok = 0, total = 0;
  mediaCount(st, &ok, &total);
  std::string media = ok == total ? strprintf("%d / %d media found", ok, total)
                                  : strprintf("%d of %d media missing \xe2\x80\x93 Locate\xe2\x80\xa6", total - ok, total);
  st->headTip = name + "  \xc2\xb7  " + (s.product.empty() ? std::string() : s.company + " " + s.product + " " + s.version + "  \xc2\xb7  ") +
                fpsText(s) + "  \xc2\xb7  " + strprintf("%g kHz / %d bit", s.sampleRate / 1000.0, s.bits) + "  \xc2\xb7  AAF starts " +
                formatTC(s.firstClip(), s.fps) + "  \xc2\xb7  " + media + "  \xc2\xb7  " + s.aafPath;
  // the update button takes room first
  const double updW = 0; // (the update note sits by the OLAAF name)
  const std::vector<int> shown = shownReels(st);
  const int nReels = (int)shown.size();
  const char *plusTip = "Add AAFs (reels) to this import, each at its own timecode. Tracks with the same name share one "
                        "REAPER track. 1, 2, 3\xe2\x80\xa6 or Shift + \xe2\x86\x90 / \xe2\x86\x92 switch between them.";
  double nmW = 0;
  if (nReels > 1) { // one tab per AAF; the details below are the shown one's
    std::vector<std::string> labels; // the AAF names (as they are: different projects stay apart)
    std::vector<bool> over;
    for (int k = 0; k < nReels; k++) {
      Importer *ri;
      ImportOptions *ro;
      labels.push_back(reelAt(st, shown[(size_t)k], &ri, &ro) ? reelLabel(ri->session(), k) : strprintf("AAF %d", k + 1));
      over.push_back(reelOverlaps(st, shown[(size_t)k]) >= 0);
    }
    const double plusW = 26, gap = 4, xW = 16;
    auto tabsW = [&](HFONT f, const std::vector<std::string> &l) {
      double w = plusW;
      for (int k = 0; k < nReels; k++) w += g.width(f, l[(size_t)k]) + 20 + (shown[(size_t)k] == st->reel ? xW : 0) + gap;
      return w;
    };
    // the whole names; if they don't fit, the part that tells them apart ("Film_Reel1_v3" -> "Reel1");
    // still too long: each name shortened
    HFONT tf = st->fBold;
    std::vector<std::string> numbered = reelNumberLabels(labels); // "Reel 1", "Reel 2": only if the names say so
    std::vector<std::string> use = numbered.empty() ? labels : numbered;
    // room for "n tracks in all" and the same / own tracks switch, as wide as they are drawn
    double room = avail - updW - (g.width(st->fSmall, "888 tracks in all") + 8) - (g.width(st->fSmall, "same tracks") + 16 + 8) - 12;
    bool fit = false;
    std::vector<std::string> shortL = reelShortLabels(labels);
    for (int pass = 0; pass < 2 && !fit; pass++) {
      if (pass == 1) use = shortL;
      for (HFONT f : {st->fBold, st->fName, st->fTiny})
        if (tabsW(f, use) <= room) {
          tf = f;
          fit = true;
          break;
        }
    }
    if (!fit) { // many AAFs: equal tabs, names shortened (narrower until they fit), at last numbers
      tf = st->fTiny;
      std::vector<std::string> base = use;
      double each = std::max(24.0, (room - plusW - xW) / nReels - gap - 20); // the shown tab also has its x
      bool fits = false;
      for (; each >= 24 && !fits; each -= 4) {
        for (int k = 0; k < nReels; k++) {
          bool c2 = false;
          std::vector<std::string> fl = fitLines(g, tf, base[(size_t)k], each, 1, &c2);
          use[(size_t)k] = fl.empty() ? base[(size_t)k] : fl[0];
        }
        fits = tabsW(tf, use) <= room;
      }
      if (!fits) // the full names are in each tab's hint
        for (int k = 0; k < nReels; k++) use[(size_t)k] = std::to_string(k + 1);
    }
    static std::vector<std::string> tabTips;
    tabTips.resize((size_t)nReels);
    double x = x0;
    for (int k = 0; k < nReels; k++) {
      int i = shown[(size_t)k];
      bool on = i == st->reel;
      double w = g.width(tf, use[(size_t)k]) + 20 + (on ? xW : 0);
      COLORREF ink = over[(size_t)k] ? kWarn : (on ? kInk : kMute);
      if (on) g.rbox(x, mid - 22, x + w, mid, 8, over[(size_t)k] ? kWarn : kBlue, kBlueSoft);
      else g.rbox(x, mid - 22, x + w, mid, 8, kLine, kFace);
      g.text(tf, ink, use[(size_t)k], x + 10, mid - 22, x + w - (on ? xW : 0) - 6, mid, DT_LEFT);
      int ov = reelOverlaps(st, i);
      std::string ovName;
      for (int q = 0; q < nReels; q++)
        if (shown[(size_t)q] == ov) ovName = labels[(size_t)q];
      tabTips[(size_t)k] = labels[(size_t)k] + (on ? "" : strprintf(": click to show it (key %d).", k + 1)) +
                           (ov >= 0 ? " It lands on top of " + ovName + " in REAPER: change where it starts (top right)."
                                    : "");
      addHit(st, x, mid - 22, x + w - (on ? xW : 0), mid, H_REEL, i, tabTips[(size_t)k].c_str());
      if (on) { // close the tab: this AAF is not imported
        g.text(st->fBody, kMute, "\xc3\x97", x + w - xW - 6, mid - 22, x + w - 4, mid, DT_CENTER);
        addHit(st, x + w - xW - 6, mid - 22, x + w, mid, H_REEL_REMOVE, i,
               "Close this tab: this AAF is not imported (the others are).");
      }
      x += w + gap;
    }
    g.rbox(x, mid - 21, x + plusW - 4, mid - 1, 10, kLine, kFace);
    g.text(st->fBody, kInk, "+", x, mid - 22, x + plusW - 4, mid - 1, DT_CENTER);
    addHit(st, x, mid - 22, x + plusW - 4, mid, H_REEL_ADD, 0, plusTip);
    x += plusW + 4;
    { // all AAFs together: how many REAPER tracks the import makes
      static std::string allTip;
      int all = totalTracks(st, &allTip);
      std::string al = strprintf("%d tracks in all", all);
      double aw = g.width(st->fSmall, al);
      g.text(st->fSmall, kMute, al, x, mid - 22, x + aw + 2, mid, DT_LEFT);
      addHit(st, x, mid - 22, x + aw + 2, mid, H_NONE, 0, allTip.c_str());
      x += aw + 4;
      // same tracks for all AAFs, or each its own: click to switch
      const char *ml = st->opt->reelsShareTracks ? "same tracks" : "own tracks";
      double mw = g.width(st->fSmall, ml) + 16;
      g.rbox(x, mid - 20, x + mw, mid - 2, 9, kLine, kFace);
      g.text(st->fSmall, kInk, ml, x, mid - 20, x + mw, mid - 2, DT_CENTER);
      addHit(st, x, mid - 21, x + mw, mid - 1, H_REEL_MODE, 0,
             st->opt->reelsShareTracks
                 ? "Same tracks: tracks with the same name share one REAPER track (each AAF's automation stays in its own time). Click: every AAF gets its own tracks."
                 : "Own tracks: every AAF gets its own REAPER tracks (the reel in the name). Click: tracks with the same name share one track.");
      x += mw + 4;
    }
    nmW = x - x0;
  } else {
    // name: the largest type that fits, else shortened (room for the + of the reels)
    const double plusW = 26;
    HFONT nf = st->fHead;
    for (HFONT f : {st->fHead, st->fBold, st->fName, st->fTiny, st->fMicro}) {
      nf = f;
      if (g.width(f, name) <= avail - updW - plusW) break;
    }
    bool cut = false;
    std::vector<std::string> nl = fitLines(g, nf, name, avail - updW - plusW, 1, &cut);
    std::string nameShown = nl.empty() ? name : nl[0];
    nmW = g.width(nf, nameShown) + 4;
    g.text(nf, kInk, nameShown, x0, mid - 20, x0 + nmW, mid + 1, DT_LEFT);
    addHit(st, x0, mid - 20, x0 + nmW, mid, H_NONE, 0, st->headTip.c_str());
    if (st->host && st->host->addReels) {
      double px = x0 + nmW + 4;
      g.rbox(px, mid - 19, px + 20, mid - 1, 9, kLine, kFace);
      g.text(st->fBody, kMute, "+", px, mid - 20, px + 20, mid - 1, DT_CENTER);
      addHit(st, px - 2, mid - 21, px + 22, mid + 1, H_REEL_ADD, 0, plusTip);
      nmW += plusW;
    }
  }
  // details: drop the least important ones, then use smaller type, until the line fits
  struct Seg {
    std::string t;
    COLORREF c;
    int hit;
    const char *tip;
    int prio; // dropped first: highest
  };
  static std::string fTip, sTip;
  fTip = strprintf("The REAPER project runs at %s fps, the AAF at %s.", fpsShort(projFps).c_str(), fpsText(s).c_str());
  sTip = strprintf("The REAPER project runs at %g kHz, the AAF at %g kHz.", projSr / 1000.0, s.sampleRate / 1000.0);
  std::vector<Seg> segs = {
      {s.product, kMute, H_NONE, nullptr, 3},
      {fpsText(s), fpsBad ? kWarn : kMute, fpsBad ? H_FPS_SHOW : H_NONE, fpsBad ? fTip.c_str() : nullptr, 0},
      {strprintf("%g kHz", s.sampleRate / 1000.0), srBad ? kWarn : kMute, srBad ? H_FPS_SHOW : H_NONE, srBad ? sTip.c_str() : nullptr, 0},
      {strprintf("%d bit", s.bits), kMute, H_NONE, nullptr, 2},
      {media, ok == total ? theme().green : kRed, ok == total ? H_NONE : H_MISS_OPEN,
       ok == total ? "Every audio file the AAF needs is there (embedded audio counts)."
                   : "Show the missing files and locate them, one by one or by folder.",
       0}};
  if (s.product.empty()) segs.erase(segs.begin());
  const std::string dot = "  \xc2\xb7  ";
  auto lineW = [&](HFONT f, int maxPrio) {
    double w = 0;
    bool first = true;
    for (auto &sg : segs)
      if (sg.prio <= maxPrio) {
        w += g.width(f, sg.t) + (first ? 0 : g.width(f, dot));
        first = false;
      }
    return w;
  };
  HFONT mf = st->fSmall;
  int maxPrio = 3;
  bool fits = false;
  for (HFONT f : {st->fSmall, st->fTiny})
    for (int p = 3; p >= 0 && !fits; p--)
      if (lineW(f, p) <= avail) {
        mf = f;
        maxPrio = p;
        fits = true;
      }
  if (!fits) {
    mf = st->fTiny;
    maxPrio = 0;
  }
  double y0 = mid + 2, y1 = mid + 19, x = x0;
  bool first = true;
  for (auto &sg : segs) {
    if (sg.prio > maxPrio) continue;
    if (!first) {
      g.text(mf, kMute, dot, x, y0, infoR, y1, DT_LEFT);
      x += g.width(mf, dot);
    }
    first = false;
    double w = g.width(mf, sg.t);
    g.text(mf, sg.c, sg.t, x, y0, infoR, y1, DT_LEFT);
    addHit(st, x, y0, std::min(infoR, x + w), y1, sg.hit, 0, sg.tip ? sg.tip : st->headTip.c_str());
    if (sg.hit == H_MISS_OPEN) st->missingX = x - 16;
    x += w;
  }
}

// volume (dB scale, -60 ... +12) and pan lines over a lane, as they would be imported
double lerpEnv(const std::vector<GainPoint> &env, double t, double def) {
  if (env.empty()) return def;
  if (t <= env.front().time) return env.front().gain;
  if (t >= env.back().time) return env.back().gain;
  for (size_t i = 1; i < env.size(); i++)
    if (t <= env[i].time) {
      double a = env[i - 1].time, b = env[i].time;
      double f = b > a ? (t - a) / (b - a) : 1.0;
      return env[i - 1].gain + (env[i].gain - env[i - 1].gain) * f;
    }
  return def;
}

// mask: 1 volume, 2 pan; `always` draws the pan line even when the track sits in the center (P on that track)
void paintAutomation(State *st, const Gfx &g, const Track &tr, double y, double spp, int mask, bool always) {
  const ImportOptions &opt = *st->opt;
  double top = y + 4, bot = y + kRowH - 5;
  auto volY = [&](double gain) {
    double db = gain > 1e-6 ? 20.0 * std::log10(gain) : -60.0;
    double n = std::max(0.0, std::min(1.0, (db + 60.0) / 72.0));
    return bot - n * (bot - top);
  };
  const COLORREF panC = RGB(0x8E, 0x44, 0xAD); // purple: orange is the DX folder color
  bool hasPan = (mask & 2) && (always || !tr.panEnv.empty() || std::fabs(tr.pan) > 1e-4);
  double px = -1, pv = 0, pp = 0;
  for (double x = kTlx + 1; x <= kTlr - 1; x += 2) {
    double t = st->viewStart + (x - kTlx) * spp;
    double v = volY(st->imp->trackVolumeAt(tr, t, opt));
    double pn = (top + bot) / 2 - lerpEnv(tr.panEnv, t, tr.pan) * (bot - top) / 2;
    if (px >= 0) {
      if (mask & 1) g.line(px, pv, x, v, kBlue);
      if (hasPan) g.line(px, pp, x, pn, panC);
    }
    px = x;
    pv = v;
    pp = pn;
  }
  // clip automation that stays on the items: a thin line inside each clip
  if ((mask & 1) && opt.clipGain && !opt.clipGainToTrack)
    for (auto &c : tr.clips) {
      if (c.excluded || c.gainEnv.empty()) continue;
      double x0 = kTlx + (c.pos - st->viewStart) / spp, x1 = kTlx + (c.pos + c.len - st->viewStart) / spp;
      double lx = -1, lv = 0;
      for (double x = std::max(x0, kTlx + 1); x <= std::min(x1, kTlr - 1); x += 2) {
        double t = st->viewStart + (x - kTlx) * spp - c.pos;
        double v = volY(c.gain * lerpEnv(c.gainEnv, t, 1.0));
        if (lx >= 0) g.line(lx, lv, x, v, kInk);
        lx = x;
        lv = v;
      }
    }
}

COLORREF mixColor(COLORREF a, COLORREF b, double t) { // t = share of b
  auto m = [&](int x, int y) { return (int)std::lround(x + (y - x) * t); };
  return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

// ------------------------------------------------------------------ stereo meter

bool clipIsStereo(State *st, const Clip &c) {
  const Session &s = st->imp->session();
  if (c.sources.size() == 2) return true;
  if (c.sources.size() == 1 && c.sources[0].channel == 0 && c.sources[0].media >= 0)
    return s.media[(size_t)c.sources[0].media].channels == 2;
  return false;
}

// open the files of a stereo event for the meter
void gonioStart(State *st, const Clip &c) {
  const Session &s = st->imp->session();
  st->rdL.close();
  st->rdR.close();
  st->gonio.clear();
  st->corr = 0;
  if (!clipIsStereo(st, c)) return;
  const Media &a = s.media[(size_t)c.sources[0].media];
  if (c.sources.size() == 2 && c.sources[1].media >= 0) {
    const Media &b = s.media[(size_t)c.sources[1].media];
    if (!st->rdL.open(a.foundPath) || !st->rdR.open(b.foundPath)) return;
    st->rdChL = std::max(0, c.sources[0].channel - 1);
    st->rdChR = std::max(0, c.sources[1].channel - 1);
  } else {
    if (!st->rdL.open(a.foundPath) || !st->rdR.open(a.foundPath)) return;
    st->rdChL = 0;
    st->rdChR = 1;
  }
  st->rdSwap = c.swapLR;
  st->rdOffset = c.srcOffset;
}

// the stretch just played: correlation (smoothed) and points for the goniometer
void gonioUpdate(State *st) {
  if (!st->rdL.isOpen() || !st->rdR.isOpen() || st->playPos < 0) return;
  const int n = 2048;
  double t = st->rdOffset + st->playPos - (double)n / std::max(1, st->rdL.rate());
  static std::vector<float> l, r;
  st->rdL.read(t, st->rdChL, n, l);
  st->rdR.read(t, st->rdChR, n, r);
  if (st->rdSwap) std::swap(l, r); // shown as it will be imported
  double lr = 0, ll = 0, rr = 0;
  for (int i = 0; i < n; i++) {
    lr += (double)l[(size_t)i] * r[(size_t)i];
    ll += (double)l[(size_t)i] * l[(size_t)i];
    rr += (double)r[(size_t)i] * r[(size_t)i];
  }
  if (ll > 1e-9 && rr > 1e-9) {
    double c = lr / std::sqrt(ll * rr);
    st->corr = st->corr * 0.7 + c * 0.3; // a little smoothing, like a meter
  }
  st->gonio.clear();
  for (int i = 0; i < n; i += 4) st->gonio.push_back({l[(size_t)i], r[(size_t)i]});
}

bool selectionStereo(State *st) {
  for (int id : st->sel)
    if (Clip *k = st->imp->findClip(id))
      if (clipIsStereo(st, *k)) return true;
  return false;
}

void paintGonio(State *st, const Gfx &g) {
  if (!st->gonioOpen || !selectionStereo(st)) return;
  double w = 248, h = 330;
  double l = kR - w - 14, t = kLB - h - 10;
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 14, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 14, kLine, kPop);
  addHit(st, l, t, l + w, t + h, H_GONIO_BG, 0,
         "Goniometer and correlation of the event that plays. Up = mono, sideways = wide, a flat line or red = "
         "one channel inverted.");
  g.text(st->fBold, kInk, "Stereo", l + 16, t + 10, l + w - 40, t + 32, DT_LEFT);
  button(st, g, l + w - 36, t + 9, l + w - 12, t + 33, "\xc3\x97", H_GONIO_CLOSE, 0, "Close the stereo meter.");
  // goniometer: a diamond, M up, S sideways, L and R on the diagonals
  double cx = l + w / 2, cy = t + 40 + 92, rad = 84;
  COLORREF grid = mixColor(kPop, kInk, 0.15);
  g.line(cx, cy - rad, cx + rad, cy, grid);
  g.line(cx + rad, cy, cx, cy + rad, grid);
  g.line(cx, cy + rad, cx - rad, cy, grid);
  g.line(cx - rad, cy, cx, cy - rad, grid);
  g.line(cx, cy - rad, cx, cy + rad, grid);
  g.line(cx - rad, cy, cx + rad, cy, grid);
  g.line(cx - rad / 2, cy - rad / 2, cx + rad / 2, cy + rad / 2, grid);
  g.line(cx + rad / 2, cy - rad / 2, cx - rad / 2, cy + rad / 2, grid);
  g.text(st->fSmall, kMute, "M", cx - 10, cy - rad - 18, cx + 10, cy - rad - 2, DT_CENTER);
  g.text(st->fSmall, kMute, "L", cx - rad / 2 - 22, cy - rad / 2 - 18, cx - rad / 2 - 4, cy - rad / 2 - 2, DT_CENTER);
  g.text(st->fSmall, kMute, "R", cx + rad / 2 + 4, cy - rad / 2 - 18, cx + rad / 2 + 22, cy - rad / 2 - 2, DT_CENTER);
  g.text(st->fSmall, kMute, "\xe2\x88\x92S", cx - rad - 22, cy - 8, cx - rad - 2, cy + 8, DT_CENTER);
  g.text(st->fSmall, kMute, "+S", cx + rad + 2, cy - 8, cx + rad + 22, cy + 8, DT_CENTER);
  // points: scaled so a loud signal fills the diamond
  float pk = 0.05f;
  for (auto &p : st->gonio) pk = std::max(pk, std::max(std::fabs(p.first), std::fabs(p.second)));
  double k = rad / (pk * 1.414);
  double dot = 1.6 / g.s;
  for (auto &p : st->gonio) {
    double m = (p.first + p.second) * 0.7071, sd = (p.second - p.first) * 0.7071;
    double x = cx + sd * k, y = cy - m * k;
    if (std::fabs(x - cx) + std::fabs(y - cy) > rad) continue;
    g.fill(x - dot, y - dot, x + dot, y + dot, kBlue);
  }
  // correlation meter: -1 ... 0 ... +1
  double by = cy + rad + 26, bl = l + 20, br = l + w - 20;
  g.text(st->fSmall, kMute, "Correlation", bl, by - 2, bl + 120, by + 14, DT_LEFT);
  double c = std::max(-1.0, std::min(1.0, st->corr));
  COLORREF cc = c < 0 ? kRed : c < 0.3 ? kWarn : theme().green;
  g.text(st->fMono, cc, strprintf("%+.2f", c), br - 70, by - 4, br, by + 16, DT_RIGHT);
  by += 18;
  g.rfill(bl, by, br, by + 14, 5, mixColor(kPop, kInk, 0.10));
  double mid = (bl + br) / 2;
  double px = mid + c * (br - bl) / 2;
  if (px > mid) g.rfill(mid, by, px, by + 14, 5, cc);
  else g.rfill(px, by, mid, by + 14, 5, cc);
  for (int q = 0; q <= 4; q++) { // ticks at -1, -0.5, 0, +0.5, +1
    double tx = bl + (br - bl) * q / 4.0;
    g.fill(tx - 0.5, by + 14, tx + 0.5, by + 18, kMute);
  }
  g.fill(mid - 1, by - 2, mid + 1, by + 16, kInk);
  g.text(st->fSmall, kMute, "\xe2\x88\x92" "1", bl - 4, by + 18, bl + 24, by + 34, DT_LEFT);
  g.text(st->fSmall, kMute, "0", mid - 10, by + 18, mid + 10, by + 34, DT_CENTER);
  g.text(st->fSmall, kMute, "+1", br - 24, by + 18, br + 4, by + 34, DT_RIGHT);
  by += 16;
  const char *what = st->playClip == 0 ? "Play the event (Shift + Space) to measure."
                     : c < 0         ? "Out of phase: one channel may be inverted."
                     : c < 0.3       ? "Wide or barely related channels."
                                     : "Coherent.";
  g.text(st->fSmall, cc, what, l + 20, by + 20, l + w - 12, by + 38, DT_LEFT);
}

// REAPER's fade shapes (fade in, t from 0 to 1)
double fadeGain(int shape, double t) {
  t = std::max(0.0, std::min(1.0, t));
  switch (shape) {
  case 1: return 1 - (1 - t) * (1 - t);           // fast start
  case 2: return t * t;                           // fast end
  case 3: return 1 - std::pow(1 - t, 4);          // fast start, steep
  case 4: return std::pow(t, 4);                  // fast end, steep
  case 5: return (1 - std::cos(kPi * t)) / 2;     // slow start and end
  case 6: {                                       // slow start and end, steep
    double u = (1 - std::cos(kPi * t)) / 2;
    return (1 - std::cos(kPi * u)) / 2;
  }
  default: return t;                              // linear
  }
}

// A fade as REAPER draws it: the curve, and the part it takes away shaded.
// a..b: where the fade runs (layout x, may lie outside the view); in = fade in.
void paintFade(const Gfx &g, double a, double b, double y0, double y1, int shape, bool in, bool shade, COLORREF shadeC,
               COLORREF lineC, bool shadeBelow = false) {
  if (b - a < 1.5) return;
  double l = std::max(a, (double)kTlx), r = std::min(b, (double)kTlr);
  if (r <= l) return;
  int n = std::max(8, std::min(96, (int)((r - l) / 2)));
  std::vector<double> curve, area;
  for (int i = 0; i <= n; i++) {
    double x = l + (r - l) * i / n;
    double t = (x - a) / (b - a);
    double gn = in ? fadeGain(shape, t) : fadeGain(shape, 1 - t);
    double y = y1 - (y1 - y0) * gn;
    curve.push_back(g.X(x));
    curve.push_back(g.Y(y));
  }
  if (shade) { // above the curve up to the top edge (or, for the hidden half of a crossfade, below it)
    area = curve;
    double ye = shadeBelow ? y1 : y0;
    area.push_back(g.X(r));
    area.push_back(g.Y(ye));
    area.push_back(g.X(l));
    area.push_back(g.Y(ye));
    olPolyFill(g.dc, area.data(), (int)area.size() / 2, shadeC);
  }
  olPolyline(g.dc, curve.data(), (int)curve.size() / 2, lineC, 0.75 * g.s); // a fine line, as in REAPER
}

// The waveform of one event: one lane per channel for mono and stereo (two small waveforms for
// L+R), multichannel events as one combined lane. Drawn as soon as the background reader has the file.
void paintWave(State *st, const Gfx &g, const Clip &c, double l, double t, double r, double b, double clipX, double spp,
               COLORREF col) {
  if (r - l < 3 || b - t < 4) return;
  struct Lane { int media, chan; };
  std::vector<std::vector<Lane>> lanes; // each lane: the channels it shows (max of them)
  for (auto &src : c.sources) {
    if (src.media < 0) continue;
    if (src.channel > 0) {
      lanes.push_back({{src.media, src.channel - 1}});
    } else {
      int n = st->peaks.channels(src.media);
      if (n <= 0) continue;
      if (c.sources.size() == 1 && n == 2) {
        if (c.chanMode == 3) lanes.push_back({{src.media, 0}});
        else if (c.chanMode == 4) lanes.push_back({{src.media, 1}});
        else {
          lanes.push_back({{src.media, 0}});
          lanes.push_back({{src.media, 1}});
        }
      } else {
        std::vector<Lane> all;
        for (int k = 0; k < n; k++) all.push_back({src.media, k});
        lanes.push_back(all);
      }
    }
  }
  if (c.swapLR && lanes.size() == 2) std::swap(lanes[0], lanes[1]); // shown as it will be imported
  if (lanes.size() > 2) { // multichannel: one combined lane
    std::vector<Lane> all;
    for (auto &ln : lanes) all.insert(all.end(), ln.begin(), ln.end());
    lanes.assign(1, all);
  }
  if (lanes.empty()) return;
  double top = t + 1, laneH = (b - t - 2) / lanes.size();
  double gain = std::max(0.0, c.gain);
  double step = 1.0 / st->scale; // one device pixel
  for (size_t i = 0; i < lanes.size(); i++) {
    double mid = top + laneH * (i + 0.5), half = laneH / 2 - (lanes.size() > 1 ? 0.5 : 0);
    if (lanes.size() > 1 && i > 0) g.fill(l + 2, top + laneH * i, r, top + laneH * i + std::max(step, 0.5), mixColor(col, kPanel, 0.6));
    for (double x = l; x < r; x += step) {
      double s0 = c.srcOffset + (x - clipX) * spp, s1 = s0 + step * spp;
      float pk = -1;
      for (auto &ln : lanes[i]) pk = std::max(pk, st->peaks.peak(ln.media, ln.chan, s0, s1));
      if (pk < 0) return; // not read yet (or not a PCM file)
      double fg = 1.0; // the fades shape the waveform too, as in REAPER
      if (st->opt->fades) {
        double tc = (x - clipX) * spp;
        if (c.fadeIn.len > 0 && tc < c.fadeIn.len) fg *= fadeGain(c.fadeIn.shape, tc / c.fadeIn.len);
        if (c.fadeOut.len > 0 && c.len - tc < c.fadeOut.len) fg *= fadeGain(c.fadeOut.shape, (c.len - tc) / c.fadeOut.len);
      }
      double a = std::sqrt(std::min(1.0, pk * gain * fg)); // square root: quiet passages stay visible
      double h = std::max(a * half, 0.5 * step);
      g.fill(x, mid - h, x + step, mid + h, col);
    }
  }
}

void paintArrange(State *st, const Gfx &g) {
  st->peakSeen = st->peaks.version();
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  buildRows(st);
  g.fill(kL, kAT, kR, kAB, kPanel); // square here; the rounded corners are put on at the end

  // header corner
  double ry = kAT + 1;
  int nsel = 0;
  for (auto &t : s.tracks) nsel += t.import && !t.created;
  int real = 0;
  for (auto &t : s.tracks) real += !t.created;
  g.fill(kL + kHdr, ry, kL + kHdr + 1, kAB - 1, kLine);
  int mOk = 0, mTotal = 0;
  mediaCount(st, &mOk, &mTotal);
  // right: Sort, Groups and + Folder; left: the count (or the red flag) and "· n empty"; each gets shorter if needed
  size_t ne = st->imp->emptyTracks().size();
  std::string el = ne ? strprintf("\xc2\xb7 %zu empty \xc3\x97", ne) : std::string();
  double ew = ne ? g.width(st->fSmall, el) + 6 : 0;
  bool sorted = opt.folders && !s.folders.empty();
  const char *sortL = sorted ? "Unsort" : "Sort";
  const char *grpL = "Sub groups";
  double sortW = g.width(st->fBody, sortL), grpW = g.width(st->fSmall, grpL);
  // no room for "n empty" next to the rest: "+ Folder" becomes "+" (its hint says what it does)
  const char *fol = "+ Folder";
  double folW = g.width(st->fBody, fol);
  if (ne && kL + 12 + g.width(st->fSmall, strprintf("%zu empty \xc3\x97", ne)) + 10 > kL + kHdr - 24 - folW - sortW - 18 - grpW - 20 - 10) {
    fol = "+";
    folW = g.width(st->fBody, fol);
  }
  g.text(st->fBody, kBlue, fol, kL + kHdr - 12 - folW, ry, kL + kHdr - 10, ry + kRuler, DT_RIGHT);
  addHit(st, kL + kHdr - 18 - folW, ry + 4, kL + kHdr - 6, ry + kRuler - 4, H_NEW_FOLDER, 0,
         "Add a folder (with the selected tracks in it). Drag track names onto a folder to put them in.");
  // Sort: into DX, FX, MX, BG by name. Once there are folders the same spot says Unsort: the AAF's order again.
  double sortR = kL + kHdr - 24 - folW;
  g.text(st->fBody, kBlue, sortL, sortR - sortW, ry, sortR, ry + kRuler, DT_RIGHT);
  addHit(st, sortR - sortW - 4, ry + 4, sortR + 4, ry + kRuler - 4, sorted ? H_SORT_RESET : H_SORT_NOW, 0,
         sorted ? "Take the tracks out of all folders and put them back in the AAF's order (Cmd/Ctrl+Z undoes it)."
                : "Sort the tracks into DX, FX, MX and BG by their names (Cmd/Ctrl+Z undoes it). The words: Options \xe2\x80\xba Sorting.");
  // Sub groups (foodgroups): numbered tracks get a folder of their own inside DX, FX, MX ... ("FX A 1", "FX A 2" -> FX > FX A)
  double gx = sortR - sortW - 18 - grpW - 20, gcy = ry + kRuler / 2.0;
  if (opt.subFolders) {
    g.rfill(gx, gcy - 7, gx + 14, gcy + 7, 4, kBlue);
    g.check(gx - 1, gcy - 8, kWhite);
  } else
    g.rbox(gx, gcy - 7, gx + 14, gcy + 7, 4, theme().empty, kPanel);
  g.text(st->fSmall, kInk, grpL, gx + 20, ry, gx + 24 + grpW, ry + kRuler, DT_LEFT);
  addHit(st, gx - 4, ry + 4, gx + 24 + grpW, ry + kRuler - 4, H_SORT_SUBS, 0,
         "Sub groups (foodgroups): Sort also puts numbered groups into folders of their own inside DX, FX, MX and BG: FX A 1, FX_A_2, fx-a3 "
         "go to FX \xe2\x80\xba FX A, FX B 1, FX B 2 to FX \xe2\x80\xba FX B. Spaces, dashes and underscores count as the same.");
  const double rightEdge = gx - 10;
  double leftEnd = kL + 12; // where "· n empty" goes
  // no audio at all: a red flag in the corner (only the flag when space is short; none without room, the
  // header says it too)
  if (mTotal > 0 && mOk == 0 && kL + 42 + ew <= rightEdge) {
    const char *fl = "No audio found";
    double fw = g.width(st->fSmall, fl) + 34;
    if (kL + 16 + fw + ew > rightEdge) {
      fl = "No audio";
      fw = g.width(st->fSmall, fl) + 34;
    }
    if (kL + 16 + fw + ew > rightEdge) {
      fl = "";
      fw = 26;
    }
    leftEnd = kL + 16 + fw;
    g.rfill(kL + 8, ry + 5, kL + 8 + fw, ry + kRuler - 5, 9, RGB(0xd2, 0x3c, 0x32));
    double fx = kL + 17, fy = ry + kRuler / 2.0;
    g.fill(fx, fy - 6, fx + 1.4, fy + 7, kWhiteFixed);                      // the pole
    g.poly({{fx + 1.4, fy - 6}, {fx + 9, fy - 3.5}, {fx + 1.4, fy - 0.5}}, kWhiteFixed); // the flag
    if (*fl) g.text(st->fSmall, kWhiteFixed, fl, kL + 30, ry + 5, kL + 8 + fw, ry + kRuler - 5, DT_LEFT);
    addHit(st, kL + 8, ry + 5, kL + 8 + fw, ry + kRuler - 5, H_MISS_OPEN, 0,
           "No audio found: none of the audio files this AAF points to was found. Click to locate them (a file or a folder).");
  } else {
    std::string cnt = strprintf("%d of %d tracks", nsel, real);
    if (kL + 18 + g.width(st->fSmall, cnt) + ew > rightEdge) cnt = strprintf("%d / %d", nsel, real);
    double cw = g.width(st->fSmall, cnt);
    if (kL + 18 + cw + ew <= rightEdge) { // no room: left out (the Import button says how many)
      g.text(st->fSmall, kMute, cnt, kL + 12, ry, kL + 14 + cw, ry + kRuler, DT_LEFT);
      addHit(st, kL + 4, ry, kL + 14 + cw, ry + kRuler, H_NONE, 0, "Tracks that will be imported, of the tracks in the AAF.");
      leftEnd = kL + 18 + cw;
    }
  }
  if (ne && leftEnd + g.width(st->fSmall, strprintf("%zu empty \xc3\x97", ne)) <= rightEdge) { // tracks with nothing at all on them: one click deletes them (undoable)
    static std::string eTip;
    if (leftEnd + ew > rightEdge) el = strprintf("%zu empty \xc3\x97", ne);
    double ex = leftEnd, ewd = g.width(st->fSmall, el);
    g.text(st->fSmall, kBlue, el, ex, ry, ex + ewd + 2, ry + kRuler, DT_LEFT);
    eTip = strprintf("%zu track%s with nothing on %s: no events (not even removed ones), no automation. Click to delete "
                     "%s before the import (Cmd/Ctrl+Z brings %s back).",
                     ne, ne == 1 ? "" : "s", ne == 1 ? "it" : "them", ne == 1 ? "it" : "them", ne == 1 ? "it" : "them");
    addHit(st, ex - 2, ry + 4, ex + ewd + 4, ry + kRuler - 4, H_REMOVE_EMPTY, 0, eTip.c_str());
  }

  // ruler
  double a, b;
  sessionRange(s, &a, &b);
  st->rangeA = a - (b - a) * 0.02;
  st->rangeB = b + (b - a) * 0.02;
  double maxStart = std::max(st->rangeA, st->rangeB - st->viewLen);
  st->viewStart = std::max(std::min(st->viewStart, maxStart), std::min(st->rangeA, maxStart));
  double tlw = kTlr - kTlx, spp = st->viewLen / tlw;
  g.fill(kTlx, ry, kR - 1, kLT - 1, theme().track);
  g.fill(kL + 1, kLT - 1, kR - 1, kLT, kLine);
  double step = niceStep(spp), off = tcOffset(st);
  for (double tt = std::floor((st->viewStart + off) / step) * step; tt <= st->viewStart + off + st->viewLen; tt += step) {
    double px = kTlx + (tt - off - st->viewStart) / spp;
    if (px < kTlx || px > kTlr) continue;
    g.fill(px, ry, px + 1, kLT, kLine);
    g.fill(px, kLT, px + 1, kLB, kSoft);
    g.text(st->fMonoSmall, kMute, formatTC(tt, s.fps), px + 4, ry, std::min(px + 110, kTlr), ry + 18, DT_LEFT);
  }
  if (opt.markers)
    for (auto &m : s.markers) {
      double px = kTlx + (m.pos - st->viewStart) / spp;
      if (px < kTlx || px > kTlr) continue;
      COLORREF mc = m.color ? rgbToRef(m.color) : kRed;
      g.fill(px, ry + 17, px + 2, kLB, mc);
      if (!m.name.empty()) {
        double w = std::min(90.0, g.width(st->fClip, m.name) + 8);
        g.fill(px, ry + 17, std::min(px + w, kTlr), ry + 29, mc);
        g.text(st->fClip, kWhiteFixed, m.name, px + 3, ry + 17, std::min(px + w, kTlr), ry + 29, DT_LEFT);
      }
    }

  // the ruler: click puts the playhead there
  addHit(st, kTlx, ry, kR - 360, kLT, H_RULER, 0, "Click to put the playhead here. Space plays from the playhead.");
  // at the right end of the ruler: play, automation view and zoom
  {
    double bx = kR - 8, by0 = ry + 5, by1 = ry + kRuler - 5;
    double cw = 0;
    g.fill(kR - 356, ry, kR - 1, kLT - 1, theme().track);
    {
      bool pl = st->playTL;
      double phT = st->playhead < 0 ? s.firstClip() : st->playhead;
      std::string tc = formatTC(phT + tcOffset(st), s.fps);
      double px0 = kR - 350;
      if (st->host && st->host->playTimeline) {
        button(st, g, px0, by0, px0 + 30, by1, pl ? "\xe2\x96\xa0" : "\xe2\x96\xb6", H_TL_PLAY, 0,
               pl ? "Stop (Space)." : "Play the timeline from the playhead (Space).", false, true, pl);
        g.text(st->fMonoSmall, pl ? kBlue : kInk, tc, px0 + 36, ry, px0 + 120, ry + kRuler, DT_LEFT);
      }
    }
    button(st, g, bx - 22, by0, bx, by1, "+", H_ZOOM_IN, 0, "Zoom in (+ key, or Alt / Option + mouse wheel). Shift + mouse wheel moves through time.");
    button(st, g, bx - 58, by0, bx - 26, by1, "Fit", H_ZOOM_FIT, 0, "Show the whole AAF.");
    button(st, g, bx - 84, by0, bx - 62, by1, "\xe2\x88\x92", H_ZOOM_OUT, 0, "Zoom out (- key, or Alt / Option + mouse wheel).");
    const char *lab = "Automation";
    cw = g.width(st->fSmall, lab);
    double ax = bx - 96 - cw - 20;
    double cy = (by0 + by1) / 2;
    if (opt.showAutomation) {
      g.rfill(ax, cy - 7, ax + 14, cy + 7, 4, kBlue);
      g.check(ax - 1, cy - 8, kWhite);
    } else
      g.rbox(ax, cy - 7, ax + 14, cy + 7, 4, theme().empty, kPanel);
    g.text(st->fSmall, kInk, lab, ax + 20, ry, ax + 24 + cw, ry + kRuler, DT_LEFT);
    addHit(st, ax - 4, by0, ax + 24 + cw, by1, H_SHOW_AUTO, 0,
           "Show the volume (blue) and pan (purple) automation of every track (A). V / P: volume / pan of the selected track or the one under the mouse; Alt / Option + V / P: of every track. A view only, it "
           "changes nothing in the import.");
  }

  // lanes
  int nrows = (int)st->rows.size();
  st->scrollRow = std::max(0, std::min(st->scrollRow, std::max(0, nrows - kRows)));
  st->lanesRc = devRect(st, kTlx, kLT, kTlr, kLB);
  st->clipHits.clear();
  addHit(st, kTlx, kLT, kTlr, kLB, H_LANES, 0,
         "Click selects, Shift-click adds. Drag or arrow up / down moves events to another track, M mutes, Delete removes.");
  bool dragging = st->drag == 3 && st->dragMoved && st->dragRows != 0;
  std::vector<std::array<double, 4>> ghosts; // where dragged events would land
  for (int vr = 0; vr < kRows; vr++) {
    int ri = st->scrollRow + vr;
    double y = kLT + vr * kRowH;
    if (ri >= nrows) continue;
    const Row &row = st->rows[(size_t)ri];
    if (row.kind == 1) { // folder
      int fi = folderIndex(s, row.folder);
      if (fi < 0) continue;
      Folder &f = s.folders[(size_t)fi];
      // a sub-folder sits indented under its folder; a folder counts the tracks of its sub-folders too
      bool sub = !f.parent.empty();
      const double fx = kL + (sub ? 16 : 0);
      auto inside = [&](const Track &t) {
        if (t.group == f.key) return true;
        const Folder *tf = s.folder(t.group);
        return tf && tf->parent == f.key;
      };
      int n = 0;
      for (auto &t : s.tracks) n += inside(t);
      g.fill(kL + 1, y, kR - 1, y + kRowH, theme().track);
      g.fill(kL + 1, y + kRowH - 1, kR - 1, y + kRowH, kLine);
      if (st->drag == 4 && st->dragMoved && dropKey(st, st->dropRow) == f.key && st->dropRow <= (int)st->rows.size() - 1)
        g.fill(kL + 1, y, kL + kHdr, y + kRowH - 1, kBlueSoft); // the folder they go into
      if (sub) { // the folder around it runs down on the left
        const Folder *pf = s.folder(f.parent);
        g.fill(kL + 1, y, kL + 4, y + kRowH, pf && pf->color ? rgbToRef(pf->color) : kMute);
      }
      g.fill(fx + 1, y, fx + 7, y + kRowH - 1, f.color ? rgbToRef(f.color) : kMute);
      addHit(st, fx + 1, y, fx + 14, y + kRowH, H_FROW_COLOR, fi, "Set the folder color.");
      bool closed = st->closedFolders.count(f.key) > 0;
      { // the arrow: pointing right when closed, down when open
        double ax = fx + 19, ay = y + kRowH / 2.0;
        if (closed) g.polyAA({{ax - 2.5, ay - 4.5}, {ax + 3.5, ay}, {ax - 2.5, ay + 4.5}}, kInk);
        else g.polyAA({{ax - 4.5, ay - 2.5}, {ax + 4.5, ay - 2.5}, {ax, ay + 3.5}}, kMute);
      }
      addHit(st, fx + 10, y + 4, fx + 28, y + kRowH - 4, H_FOLD_TOGGLE, fi,
             closed ? "Open the folder (Alt / Option-click: all folders)." : "Close the folder (Alt / Option-click: all folders). Its tracks still import.");
      if (closed) { // its events, small, so the folder's content stays readable
        COLORREF fc = f.color ? rgbToRef(f.color) : kMute;
        double spp2 = st->viewLen / (kTlr - kTlx);
        for (auto &t : s.tracks)
          if (inside(t) && t.import)
            for (auto &c : t.clips) {
              if (c.excluded) continue;
              double x0 = std::max((double)kTlx, kTlx + (c.pos - st->viewStart) / spp2);
              double x1 = std::min((double)kTlr, kTlx + (c.pos + c.len - st->viewStart) / spp2);
              if (x1 > x0) g.fill(x0, y + kRowH / 2.0 - 4, std::max(x0 + 1, x1), y + kRowH / 2.0 + 4, mixColor(fc, kPanel, 0.35));
            }
      }
      bool renaming = st->editKind == 2 && st->editKey == f.key;
      double nw = std::min(sub ? 104.0 : 120.0, g.width(st->fBold, f.name) + 12);
      if (!renaming) g.text(st->fBold, kInk, f.name, fx + 28, y, fx + 28 + nw, y + kRowH, DT_LEFT);
      { // "folder · 12 tracks", shorter when the row is narrow
        double room = kL + kHdr - 30 - (fx + 34 + nw);
        std::string what = strprintf("folder \xc2\xb7 %d track%s", n, n == 1 ? "" : "s");
        if (g.width(st->fSmall, what) > room) what = strprintf("%d track%s", n, n == 1 ? "" : "s");
        if (g.width(st->fSmall, what) > room) what = std::to_string(n);
        g.text(st->fSmall, kMute, what, fx + 34 + nw, y, kL + kHdr - 30, y + kRowH, DT_LEFT);
      }
      if (!renaming) // a click on the name renames the folder, like the pencil
        addHit(st, fx + 28, y + 4, kL + kHdr - 30, y + kRowH - 4, H_FROW_RENAME, fi, "Click to rename this folder.");
      g.icon("\xe2\x9c\x8e", kL + kHdr - 17, y + kRowH / 2.0 - 1, kMute, kSoft);
      addHit(st, kL + kHdr - 28, y + 4, kL + kHdr - 4, y + kRowH - 4, H_FROW_RENAME, fi, "Rename this folder.");
      continue;
    }
    g.fill(kL + 1, y + kRowH - 1, kR - 1, y + kRowH, kSoft);
    if (row.kind == 2) { // drop line for new tracks
      bool hot = dragging && posAtRow(st, ri) <= posAtRow(st, st->scrollRow + kRows) &&
                 st->dragFromPos + st->dragRows >= (int)st->order.size();
      bool trackDrop = st->drag == 4 && st->dragMoved && st->dropRow > ri;
      if (trackDrop) g.fill(kL + 1, y, kL + kHdr, y + kRowH - 1, kBlueSoft);
      g.text(st->fBody, hot || trackDrop ? kBlue : kMute, trackDrop ? "Outside any folder" : "+ Drop here for a new track",
             kL + 30, y, kL + kHdr - 6, y + kRowH, DT_LEFT);
      continue;
    }
    Track &tr = s.tracks[(size_t)row.track];
    bool on = tr.import;
    uint32_t eff = st->imp->trackColor(tr, opt);
    COLORREF tc = eff ? rgbToRef(eff) : RGB(0x7f, 0x9c, 0xd8);
    // tracks inside a folder are indented, with the folder's color running down on the left
    const Folder *inF = (opt.folders && !tr.group.empty()) ? s.folder(tr.group) : nullptr;
    const Folder *outF = inF && !inF->parent.empty() ? s.folder(inF->parent) : nullptr; // the folder around a sub-folder
    double ind = inF ? (outF ? 32 : 16) : 0, hx = kL + ind;
    if (!on) g.fill(kL + 1, y, kR - 1, y + kRowH - 1, theme().track);
    if (outF) g.fill(kL + 1, y, kL + 4, y + kRowH, outF->color ? rgbToRef(outF->color) : kMute);
    if (inF) g.fill(kL + (outF ? 17 : 1), y, kL + (outF ? 20 : 4), y + kRowH, inF->color ? rgbToRef(inF->color) : kMute);
    bool trSel = st->selTracks.count(row.track) > 0;
    bool grabbed = trSel && st->drag == 4 && st->dragMoved;
    if (trSel) g.fill(hx + 7, y, kL + kHdr, y + kRowH - 1, kBlueSoft);
    g.fill(hx + 1, y, hx + 7, y + kRowH - 1, on ? tc : theme().off);
    addHit(st, hx + 1, y, hx + 14, y + kRowH, H_TRACK_COLOR, row.track,
           "Set the track color. Without one, a track takes the color of its folder.");
    double cy = y + (kRowH - 18) / 2;
    if (on) {
      g.rfill(hx + 20, cy + 1, hx + 36, cy + 17, 4, kBlue);
      g.check(hx + 20, cy + 1, kWhite);
    } else
      g.rbox(hx + 20, cy + 1, hx + 36, cy + 17, 4, theme().empty, kPanel);
    addHit(st, hx + 16, y + 2, hx + 42, y + kRowH - 2, H_TRACK_CHECK, row.track, "Import this track or leave it out.");
    double nameR = tr.created ? kL + kHdr - 34 : kL + kHdr - 8;
    { // solo: for listening here only
      bool so = st->solo.count(tr.uid) > 0;
      double sr = nameR, sl = sr - 22;
      if (so || st->hoverRow == ri || trSel) {
        COLORREF soC = RGB(0xe8, 0xc5, 0x47);
        if (so) {
          g.rfill(sl, y + 8, sr, y + kRowH - 8, 5, soC);
          g.text(st->fSmall, RGB(0x22, 0x1c, 0x08), "S", sl, y + 8, sr, y + kRowH - 8, DT_CENTER);
        } else {
          g.rbox(sl, y + 8, sr, y + kRowH - 8, 5, kLine, kPanel);
          g.text(st->fSmall, kMute, "S", sl, y + 8, sr, y + kRowH - 8, DT_CENTER);
        }
      }
      addHit(st, sl, y + 6, sr, y + kRowH - 6, H_TRACK_SOLO, row.track,
             so ? "Solo is on: only this track plays. Click to turn it off (Shift + S)."
                : "Solo: only this track plays (Shift + S for the selected event's track). One track at a time; listening only, "
                  "the import is not changed.");
      nameR = sl - 4;
    }
    bool hot = st->hoverRow == ri || trSel; // the track's tools show under the mouse
    if (st->host && st->host->projectTracks) { // put the events on an existing REAPER track
      bool matched = !tr.target.empty();
      if (hot || matched) g.icon("\xe2\x87\xa5", nameR - 9, y + kRowH / 2.0, matched ? kBlue : kMute, kPanel);
      addHit(st, nameR - 18, y + 4, nameR, y + kRowH - 4, H_TRACK_DEST, row.track,
             "Put this track on a track that is already in your REAPER project (or on a new one).");
      nameR -= 18;
    }
    if (opt.folders) {
      if (hot) g.icon("\xe2\x96\xbe", nameR - 9, y + kRowH / 2.0, kMute, kPanel);
      addHit(st, nameR - 18, y + 4, nameR, y + kRowH - 4, H_TRACK_FOLDERMENU, row.track, "Choose the folder for this track.");
      nameR -= 18;
    }
    { // L+R: combine an .L / .R pair into one stereo track, or split it again (right-click: swap L / R)
      bool sw = false;
      for (auto &c : tr.clips) sw = sw || c.swapLR;
      bool combined = tr.channels == 2 && !tr.mergedFrom.empty();
      int partner = combined ? -1 : st->imp->stereoPartner(row.track);
      if (st->stereoTips.size() < s.tracks.size()) st->stereoTips.resize(s.tracks.size());
      std::string &tip = st->stereoTips[(size_t)row.track];
      if (combined || partner >= 0) {
        const char *badge = sw ? "R+L" : "L+R";
        if (combined)
          g.rbox(nameR - 30, y + 11, nameR - 2, y + 25, 4, sw ? kWarn : theme().blueLine, sw ? mixColor(kWarn, kPanel, 0.8) : kBlueSoft);
        else
          g.rbox(nameR - 30, y + 11, nameR - 2, y + 25, 4, kLine, kPanel);
        g.text(st->fClip, combined ? (sw ? kWarn : kBlue) : kMute, badge, nameR - 30, y + 11, nameR - 2, y + 25, DT_CENTER);
        tip = combined ? "One stereo track from " + tr.mergedFrom + ". Click to split it into the two mono tracks again." +
                             (sw ? " Left and right are exchanged." : "") + " Right-click the name to swap left and right."
                       : "Click to combine " + tr.name + " and " + s.tracks[(size_t)partner].name + " into one stereo track.";
        addHit(st, nameR - 30, y + 9, nameR - 2, y + 27, H_TRACK_STEREO, row.track, tip.c_str());
        nameR -= 34;
      } else if (tr.channels >= 2) { // a stereo (or multichannel) file: information only
        const char *badge = tr.channels == 2 ? (sw ? "R/L" : "ST") : "MC";
        g.text(st->fClip, sw ? kWarn : kMute, badge, nameR - 30, y + 11, nameR - 2, y + 25, DT_CENTER);
        tip = tr.channels == 2 ? std::string(sw ? "Stereo, left and right exchanged." : "Stereo.") +
                                     " Right-click the name to swap left and right for the whole track."
                               : strprintf("%d channels.", tr.channels);
        addHit(st, nameR - 30, y + 9, nameR - 2, y + 27, H_NONE, 0, tip.c_str());
        nameR -= 34;
      }
    }
    COLORREF nameC = (on && !grabbed) ? kInk : kMute;
    if (st->editKind == 4 && st->editTrack == row.track) nameC = trSel ? kBlueSoft : kPanel; // the editor sits on top
    double nx = hx + 48;
    // the name: smaller regular type; a long name takes two lines, the full name shows in the footer
    std::string sub = !tr.target.empty() ? "\xe2\x86\x92 " + tr.targetName + (opt.overwriteEnvelopes ? "  (envelopes overwritten)" : "")
                                         : tr.mergedFrom;
    // renamed: the AAF's name stays readable underneath, in gray
    if (!tr.aafName.empty() && tr.name != tr.aafName && tr.target.empty())
      sub = tr.aafName + (tr.mergedFrom.empty() ? std::string() : "  (" + tr.mergedFrom + ")");
    if (sub.empty() && st->imp->trackIsEmpty(row.track)) sub = "empty";
    bool cutName = false, cutSub = false;
    std::vector<std::string> nl = fitLines(g, st->fName, tr.name, nameR - nx, sub.empty() ? 2 : 1, &cutName);
    if (nl.empty()) nl.push_back(std::string());
    if (sub.empty() && nl.size() == 1)
      g.text(st->fName, nameC, nl[0], nx, y, nameR, y + kRowH, DT_LEFT);
    else if (sub.empty()) {
      g.text(st->fName, nameC, nl[0], nx, y + 3, nameR, y + 18, DT_LEFT);
      g.text(st->fName, nameC, nl[1], nx, y + 17, nameR, y + kRowH - 2, DT_LEFT);
    } else {
      g.text(st->fName, nameC, nl[0], nx, y + 3, nameR, y + 19, DT_LEFT);
      std::vector<std::string> sl = fitLines(g, st->fTiny, sub, nameR - nx, 1, &cutSub);
      g.text(st->fTiny, !tr.target.empty() ? kBlue : kMute, sl.empty() ? "" : sl[0], nx, y + 19, nameR, y + kRowH - 3, DT_LEFT);
    }
    if (!on && nl.size() == 1 && sub.empty())
      g.fill(nx, y + kRowH / 2, std::min(nameR, nx + g.width(st->fName, nl[0])), y + kRowH / 2 + 1, kMute);
    if (st->nameTips.size() < s.tracks.size()) st->nameTips.resize(s.tracks.size());
    std::string &nt = st->nameTips[(size_t)row.track];
    nt = (cutName || cutSub) ? tr.name + (sub.empty() ? "" : "  \xc2\xb7  " + sub) + "   \xe2\x80\x94  " : std::string();
    nt += "Click selects, Shift or Cmd / Ctrl-click adds, double-click renames. Drag over names to select several, drag selected "
          "names to move them.";
    if (grabbed) g.fill(kL + kHdr - 3, y, kL + kHdr, y + kRowH - 1, kBlue); // picked up
    addHit(st, hx + 44, y + 2, nameR, y + kRowH - 2, H_TRACK_NAME, row.track, nt.c_str());
    if (tr.created) { // a track added here can be deleted again; AAF tracks cannot
      button(st, g, kL + kHdr - 28, y + 8, kL + kHdr - 8, y + kRowH - 8, "\xc3\x97", H_TRACK_DELETE, row.track,
             "Delete this new track. Its events go back to the AAF tracks they came from.");
    }

    // events
    for (auto &c : tr.clips) {
      double x0 = kTlx + (c.pos - st->viewStart) / spp, x1 = kTlx + (c.pos + c.len - st->viewStart) / spp;
      if (x1 < kTlx || x0 > kTlr) continue;
      x0 = std::max(x0, kTlx + 1);
      x1 = std::min(x1, kTlr - 1);
      if (x1 - x0 < 1) x1 = x0 + 1;
      bool selected = st->sel.count(c.id) > 0;
      double y0 = y + 4, y1 = y + kRowH - 4;
      double wy = (x1 - x0 > 30 && !c.name.empty()) ? y0 + 11 : y0; // waveform under the name
      if (selected && dragging) { // ghost where the event would land
        int lands = st->dragBlocked ? st->dragRows : st->dragEff;
        int tr2 = rowOfPos(st, st->dragFromPos + lands + (posOfTrack(st, row.track) - st->dragFromPos));
        int gv = tr2 - st->scrollRow;
        if (gv >= 0 && gv < kRows) {
          double gy = kLT + gv * kRowH;
          ghosts.push_back({x0, gy + 4, x1, gy + kRowH - 4}); // drawn over all rows, below
        }
      }
      COLORREF cc = on ? tc : theme().off;
      // a light tint of the track color, the waveform in a deeper shade of it
      COLORREF tint = mixColor(cc, kPanel, olaaf::darkTheme() ? 0.55 : 0.72);
      COLORREF wave = olaaf::darkTheme() ? mixColor(cc, RGB(255, 255, 255), 0.15) : mixColor(cc, RGB(0, 0, 0), 0.25);
      double cx0 = kTlx + (c.pos - st->viewStart) / spp; // unclipped left edge, for the waveform time
      if (c.excluded) {
        g.box(x0, y0, x1, y1, selected ? kInk : kLine, kPanel);
        g.line(x0, y1, x1, y0, kLine);
      } else if (c.mute) {
        if (selected) g.fill(x0 - 2, y0 - 2, x1 + 2, y1 + 2, kInk);
        g.box(x0, y0, x1, y1, cc, kPanel);
        paintWave(st, g, c, x0 + 1, std::max(wy, y0 + 1), x1 - 1, y1 - 1, cx0, spp, mixColor(cc, kPanel, 0.55));
      } else {
        if (selected) g.rfill(x0 - 2, y0 - 2, x1 + 2, y1 + 2, 6, kInk);
        if (selected) g.rfill(x0 - 1, y0 - 1, x1 + 1, y1 + 1, 5, kPanel);
        if (x1 - x0 > 8) {
          g.rfill(x0, y0, x1, y1, 4, cc);            // the clip start shows as a colored left edge
          g.rfill(x0 + 2, y0, x1, y1, 4, tint);
          g.fill(x0 + 2, y0, std::min(x1, x0 + 6), y1, tint);
        } else
          g.fill(x0, y0, x1, y1, tint);
        paintWave(st, g, c, x0 + 2, wy, x1 - 1, y1, cx0, spp, wave);
        if (opt.fades) { // smooth fades, the faded part darker (crossfade lines are drawn once the track is done)
          double cx1 = cx0 + c.len / spp, fi = c.fadeIn.len / spp, fo = c.fadeOut.len / spp;
          COLORREF sh = mixColor(tint, olaaf::darkTheme() ? RGB(0, 0, 0) : kPanel, olaaf::darkTheme() ? 0.45 : 0.6);
          paintFade(g, cx0, cx0 + fi, y0, y1, c.fadeIn.shape, true, true, sh, kFadeLine);
          paintFade(g, cx1 - fo, cx1, y0, y1, c.fadeOut.shape, false, true, sh, kFadeLine);
        }
      }
      // a file not found: a red corner (no waveform can come from it)
      bool missingFile = false;
      for (auto &src : c.sources)
        if (src.media >= 0 && src.media < (int)s.media.size() && s.media[(size_t)src.media].foundPath.empty() &&
            s.media[(size_t)src.media].kind != MediaKind::Embedded)
          missingFile = true;
      if (missingFile && !c.excluded) {
        double k = std::min(12.0, std::max(2.0, x1 - x0)); // never wider than the event itself
        g.polyAA({{x1 - k, y0}, {x1, y0}, {x1, y0 + k}}, kRed);
      }
      if (x1 - x0 > 30 && !c.name.empty())
        g.text(st->fClip, (c.excluded || c.mute || !on) ? kMute : kInk, c.mute ? "M  " + c.name : c.name, x0 + 4, y0,
               x1 - (missingFile ? 13 : 3), y0 + 12, DT_LEFT);
      ClipHit h;
      h.r = devRect(st, x0, y0, x1, y1);
      h.id = c.id;
      st->clipHits.push_back(h);
    }
    // crossfades: where two events overlap, both curves show as an X on top
    if (opt.fades)
      for (size_t i = 0; i < tr.clips.size(); i++) {
        const Clip &p = tr.clips[i];
        if (p.excluded || p.mute || p.pos + p.len < st->viewStart || p.pos > st->viewStart + st->viewLen) continue;
        for (size_t j = 0; j < tr.clips.size(); j++) {
          const Clip &q = tr.clips[j];
          if (j == i || q.excluded || q.mute || q.pos <= p.pos || q.pos >= p.pos + p.len) continue;
          double y0 = y + 4, y1 = y + kRowH - 4;
          double pe = kTlx + (p.pos + p.len - st->viewStart) / spp, qs = kTlx + (q.pos - st->viewStart) / spp;
          double fo = std::max(p.fadeOut.len / spp, pe - qs), fi = std::max(q.fadeIn.len / spp, pe - qs);
          // the earlier event shows under its fade-out curve again, so only the part both fades take away stays dark
          COLORREF pc = on ? tc : theme().off;
          COLORREF ptint = mixColor(pc, kPanel, olaaf::darkTheme() ? 0.55 : 0.72);
          bool inside = pe - fo >= qs - 1; // the fade-out lies within the overlap
          paintFade(g, pe - fo, pe, y0, y1, p.fadeOut.shape, false, inside, ptint, kFadeLine, true);
          paintFade(g, qs, qs + fi, y0, y1, q.fadeIn.shape, true, false, 0, kFadeLine);
          double qe = std::min((double)kTlr, kTlx + (q.pos + q.len - st->viewStart) / spp), qx = std::max((double)kTlx, qs);
          if (qe - qx > 30 && !q.name.empty()) g.text(st->fClip, kInk, q.name, qx + 4, y0, qe - 3, y0 + 12, DT_LEFT);
        }
      }
    if (on) { // A / the Automation box: every track; V / P: only the tracks they were pressed on
      auto ls = st->laneShow.find(laneKey(st, tr.uid));
      int mine = ls == st->laneShow.end() ? 0 : ls->second;
      if (opt.showAutomation || mine) paintAutomation(st, g, tr, y, spp, opt.showAutomation ? 3 | mine : mine, mine & 2);
    }
  }
  for (auto &gh : ghosts) { // red: that spot is taken
    if (st->dragBlocked) { // a hole with a red frame and a cross: not here
      g.rfill(gh[0] - 2, gh[1] - 2, gh[2] + 2, gh[3] + 2, 6, kRed);
      g.rfill(gh[0], gh[1], gh[2], gh[3], 4, kPanel);
      g.line(gh[0] + 3, gh[1] + 3, gh[2] - 3, gh[3] - 3, kRed);
      g.line(gh[0] + 3, gh[3] - 3, gh[2] - 3, gh[1] + 3, kRed);
    }
    else g.rbox(gh[0], gh[1], gh[2], gh[3], 4, kBlue, kBlueSoft);
  }

  // the timeline playhead
  {
    double phT = st->playhead < 0 ? s.firstClip() : st->playhead;
    double px = kTlx + (phT - st->viewStart) / spp;
    if (px >= kTlx && px <= kTlr && !st->playClip) {
      COLORREF pc = st->playTL ? kBlue : kMute;
      g.fill(px - (st->playTL ? 1 : 0.5), kLT, px + (st->playTL ? 1 : 0.5), kLB, pc);
      g.poly({{px - 5, kLT - 8}, {px + 5, kLT - 8}, {px, kLT - 1}}, pc);
    }
  }
  // listening: a line moving through the event
  if (st->playClip && st->playPos >= 0)
    if (Clip *pc = st->imp->findClip(st->playClip)) {
      double px = kTlx + (pc->pos + st->playPos - st->viewStart) / spp;
      if (px >= kTlx && px <= kTlr) {
        g.fill(px - 1, kLT, px + 1, kLB, kBlue);
        g.poly({{px - 5, kLT - 7}, {px + 5, kLT - 7}, {px, kLT}}, kBlue);
      }
    }

  // moving tracks: a line where they will land, and what is being carried
  if (st->drag == 4 && st->dragMoved && st->dropRow >= 0) {
    int slot = std::min(st->dropRow, nrows - 1);
    double ly = kLT + (slot - st->scrollRow) * kRowH;
    if (ly >= kLT - 1 && ly <= kLB + 1) {
      std::string key = dropKey(st, st->dropRow);
      const Folder *kf = opt.folders && !key.empty() ? s.folder(key) : nullptr;
      double ix = kL + (kf ? (kf->parent.empty() ? 16 : 32) : 0);
      g.fill(ix + 1, ly - 1, kTlr, ly + 2, kBlue);
      g.fill(ix + 1, ly - 4, ix + 4, ly + 5, kBlue);
    }
    std::string what = st->selTracks.size() == 1 ? s.tracks[(size_t)*st->selTracks.begin()].name
                                                 : strprintf("%zu tracks", st->selTracks.size());
    double my = (st->dragMouseY - st->oy) / st->scale;
    double w = std::min(200.0, g.width(st->fSmall, what) + 16);
    g.fill(kL + 60, my + 8, kL + 60 + w, my + 28, kBlue);
    g.text(st->fSmall, kWhiteFixed, what, kL + 68, my + 8, kL + 60 + w - 4, my + 28, DT_LEFT);
  }

  // vertical scrollbar
  double vl = kTlr, vr2 = kR - 1, vt = kLT, vb = kLB;
  g.fill(vl, vt, vr2, vb, theme().track);
  g.fill(vl, vt, vl + 1, vb, kSoft);
  double th = nrows > kRows ? std::max(24.0, (vb - vt) * kRows / nrows) : (vb - vt);
  double tp = nrows > kRows ? vt + (vb - vt - th) * st->scrollRow / (nrows - kRows) : vt;
  g.fill(vl + 3, tp + 2, vr2 - 2, tp + th - 2, nrows > kRows ? theme().thumb : kSoft);
  st->vTrack = devRect(st, vl, vt, vr2, vb);
  st->vThumb = devRect(st, vl, tp, vr2, tp + th);
  addHit(st, vl, vt, vr2, vb, H_NONE, 0, "Drag to scroll through the tracks. The mouse wheel works too.");

  // horizontal scrollbar
  double hl = kTlx, hr = kTlr, ht = kLB + 1, hb = kLB + 1 + kSb;
  g.fill(kL + 1, kLB, kR - 1, kLB + 1, kLine);
  g.fill(hl, ht, hr, hb, theme().track);
  double total = st->rangeB - st->rangeA;
  double frac = std::min(1.0, st->viewLen / std::max(1e-9, total));
  double tw = std::max(24.0, (hr - hl) * frac);
  double pos = total > st->viewLen ? (st->viewStart - st->rangeA) / (total - st->viewLen) : 0;
  pos = std::max(0.0, std::min(1.0, pos));
  double tx = hl + (hr - hl - tw) * pos;
  g.fill(tx + 2, ht + 3, tx + tw - 2, hb - 2, frac < 1.0 ? theme().thumb : kSoft);
  st->hTrack = devRect(st, hl, ht, hr, hb);
  st->hThumb = devRect(st, tx, ht, tx + tw, hb);
  addHit(st, hl, ht, hr, hb, H_NONE, 0, "Drag to move through time, or use + and \xe2\x88\x92 to zoom.");
  if (st->marquee) { // the selection frame
    double l = (std::min(st->mqX0, st->mqX1) - g.ox) / g.s, r = (std::max(st->mqX0, st->mqX1) - g.ox) / g.s;
    double t = (std::min(st->mqY0, st->mqY1) - g.oy) / g.s, mb = (std::max(st->mqY0, st->mqY1) - g.oy) / g.s;
    g.fill(l, t, r, t + 1, kBlue);
    g.fill(l, mb - 1, r, mb, kBlue);
    g.fill(l, t, l + 1, mb, kBlue);
    g.fill(r - 1, t, r, mb, kBlue);
  }
  olRoundCorners(g.dc, g.X(kL), g.Y(kAT), g.X(kR), g.Y(kAB), 12 * g.s, kFace, kLine);
}

// The bar under the arrange view: the selected events and what to do with them, or the help line.
void paintEventBar(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  double t = kEvT, b = kEvB, mid = (t + b) / 2;
  std::vector<int> ids(st->sel.begin(), st->sel.end());
  double right = kR;
  if (!ids.empty()) {
    bool anyIn = false, allMuted = true;
    for (int id : ids)
      if (Clip *k = st->imp->findClip(id)) {
        anyIn = anyIn || !k->excluded;
        allMuted = allMuted && k->mute;
      }
    double aw = 136;
    checkbox(st, g, right - aw, mid - 8, st->moveAutomation, H_EV_AUTOMATION, 0,
             "Volume and pan automation under a moved event goes with it to the new track.");
    g.text(st->fBody, kInk, "Automation along", right - aw + 22, t, right, b, DT_LEFT);
    right -= aw + 12;
    if (st->edited) {
      button(st, g, right - 70, mid - 15, right, mid + 15, "Reset", H_EV_RESET, 0, "Undo every remove, mute and move made here.");
      right -= 78;
    }
    button(st, g, right - 32, mid - 15, right, mid + 15, "\xe2\x96\xbc", H_EV_DOWN, 0,
           "One track down (arrow down). Below the last track a new track is added.");
    button(st, g, right - 68, mid - 15, right - 36, mid + 15, "\xe2\x96\xb2", H_EV_UP, 0, "One track up (arrow up).");
    right -= 76;
    const char *rl = anyIn ? "Remove" : "Restore";
    double rw = g.width(st->fBody, rl) + 26;
    button(st, g, right - rw, mid - 15, right, mid + 15, rl, anyIn ? H_EV_REMOVE : H_EV_RESTORE, 0,
           anyIn ? "Leaves the selected events out (Delete). They stay visible and can be restored." :
                   "Puts the selected events back into the import (Delete).");
    right -= rw + 6;
    const char *ml = allMuted ? "Unmute" : "Mute";
    double mw = g.width(st->fBody, ml) + 26;
    button(st, g, right - mw, mid - 15, right, mid + 15, ml, H_EV_MUTE, 0, "Imports the selected events as muted items (M).");
    right -= mw + 6;
    {
      bool any = false, all = true;
      for (int sid : ids)
        if (Clip *k = st->imp->findClip(sid))
          if (clipIsStereo(st, *k)) {
            any = true;
            all = all && k->swapLR;
          }
      if (any) {
        const char *sl = "Swap L/R";
        double sw = g.width(st->fBody, sl) + 26;
        button(st, g, right - sw, mid - 15, right, mid + 15, sl, H_EV_SWAP, 0,
               all ? "Left and right are exchanged on import. Click to put them back."
                   : "Exchange left and right of the selected stereo events (on import and when listening).",
               false, true, all);
        right -= sw + 6;
        const char *gl = "Meter";
        double gw = g.width(st->fBody, gl) + 26;
        button(st, g, right - gw, mid - 15, right, mid + 15, gl, H_GONIO_TOGGLE, 0,
               st->gonioOpen ? "Hide the stereo meter (G)." : "Show the goniometer and correlation meter; Shift + Space plays the event (G).",
               false, true, st->gonioOpen);
        right -= gw + 6;
      }
    }
    if (st->host && st->host->playPreview) {
      bool pl = st->playClip != 0;
      button(st, g, right - 34, mid - 15, right, mid + 15, pl ? "\xe2\x96\xa0" : "\xe2\x96\xb6", H_EV_PLAY, 0,
             pl ? "Stop (Shift + Space)." : "Listen to this event (Shift + Space).", false, true, pl);
      right -= 48;
    }

  } else if (st->edited) {
    button(st, g, right - 96, mid - 15, right, mid + 15, "Reset edits", H_EV_RESET, 0, "Undo every remove, mute and move made here.");
    right -= 110;
  }
  double x = kL;
  if (ids.empty()) {
    g.text(st->fBody, kMute, "No event selected. Click one to listen, mute, remove, move or swap it.", x, t, right, b, DT_LEFT);
    return;
  }
  int ti = -1;
  Clip *c = st->imp->findClip(ids[0], &ti);
  std::string nm, det;
  if (c && ids.size() == 1) {
    nm = c->name.empty() ? "(no name)" : c->name;
    det = strprintf("%s \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s%s%s", ti >= 0 ? s.tracks[(size_t)ti].name.c_str() : "-",
                    formatTC(c->pos + tcOffset(st), s.fps).c_str(), formatTC(c->len, s.fps).c_str(), dbText(c->gain).c_str(),
                    c->gainEnv.empty() ? "" : " + curve",
                    c->excluded ? " \xc2\xb7 removed" : (c->mute ? " \xc2\xb7 muted" : ""));
    if (st->showSamples && s.sampleRate > 0)
      det += strprintf(" \xc2\xb7 %lld / %lld smp", (long long)std::llround((c->pos + tcOffset(st)) * s.sampleRate),
                       (long long)std::llround(c->len * s.sampleRate));
  } else {
    std::set<int> trs;
    for (int id : ids) {
      int k = -1;
      if (st->imp->findClip(id, &k)) trs.insert(k);
    }
    nm = strprintf("%zu events on %zu track%s", ids.size(), trs.size(), trs.size() == 1 ? "" : "s");
  }
  // the file name in regular type over the whole room; a long one takes the second line too (the details
  // then show in the footer on hover), otherwise the details sit on the second line
  double room = right - 12 - x;
  bool cutN = false;
  std::vector<std::string> nl = fitLines(g, st->fName, nm, room, 2, &cutN);
  if (nl.empty()) nl.push_back(nm);
  if (nl.size() == 1) {
    g.text(st->fName, kInk, nl[0], x, t + 3, x + room, mid + 1, DT_LEFT);
    bool cutD = false;
    std::vector<std::string> dl = fitLines(g, st->fMonoSmall, det, room, 1, &cutD);
    g.text(st->fMonoSmall, kMute, dl.empty() ? det : dl[0], x, mid + 1, x + room, b - 3, DT_LEFT);
  } else {
    g.text(st->fName, kInk, nl[0], x, t + 3, x + room, mid + 1, DT_LEFT);
    g.text(st->fName, kInk, nl[1], x, mid, x + room, b - 3, DT_LEFT);
  }
  static std::string evTip;
  evTip = nm + "   " + det;
  addHit(st, x, t, x + room, b, H_NONE, 0, evTip.c_str());
}

// ------------------------------------------------------------------ options

const double kOpW = 380;

void optRow(State *st, const Gfx &g, double px, double &y, const std::string &label, bool on, bool enabled, int id,
            const char *tip) {
  if (!enabled) g.rbox(px + 16, y + 6, px + 32, y + 22, 4, kLine, kPanel);
  else if (on) {
    g.rfill(px + 16, y + 6, px + 32, y + 22, 4, kBlue);
    g.check(px + 16, y + 6, kWhite);
  } else
    g.rbox(px + 16, y + 6, px + 32, y + 22, 4, theme().empty, kPanel);
  g.text(st->fBody, enabled ? kInk : kMute, label, px + 42, y, px + kOpW - 16, y + 28, DT_LEFT);
  addHit(st, px + 10, y + 1, px + kOpW - 10, y + 27, enabled ? id : H_OPTS_BG, 0, tip);
  y += 28;
}

void optHead(State *st, const Gfx &g, double px, double &y, const char *t) {
  y += 8;
  g.text(st->fSmall, kMute, t, px + 16, y, px + kOpW - 16, y + 20, DT_LEFT);
  y += 20;
}

void paintOptions(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double px = std::min(st->optsBtn.r - 60, kR - kOpW), py = kTopH - 6;
  double ph = 426;
  // shadow, then the panel
  g.rfill(px - 1, py + 1, px + kOpW + 3, py + ph + 5, 12, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(px, py, px + kOpW, py + ph, 12, kLine, kPop);
  addHit(st, px, py, px + kOpW, py + ph, H_OPTS_BG, 0, nullptr);
  double y = py + 4;
  optHead(st, g, px, y, "Import");
  optRow(st, g, px, y, "Fades and crossfades", opt.fades, true, H_FADES, "Fade ins, fade outs and crossfades from the AAF.");
  optRow(st, g, px, y, "Combine .L / .R tracks into stereo", st->merge, true, H_MERGE,
         "Combines two mono tracks named .L and .R whose clips line up into one stereo track.");
  std::string mLabel = s.markers.empty() ? "Markers (none in this AAF)" : strprintf("Markers (%zu)", s.markers.size());
  optRow(st, g, px, y, mLabel, opt.markers && !s.markers.empty(), !s.markers.empty(), H_MARKERS,
         "Markers from the edit become REAPER markers.");
  {
    double save = y;
    optRow(st, g, px, y, "Folder tracks", opt.folders, true, H_FOLDERS,
           "Folder tracks. Click a track name to choose its folder; the pencil renames a folder.");
    std::string rl = opt.trackTemplate.empty() ? "Sorting\xe2\x80\xa6" : "Sorting + template\xe2\x80\xa6";
    double bw = g.width(st->fBody, rl) + 24;
    button(st, g, px + kOpW - 16 - bw, save + 1, px + kOpW - 16, save + 27, rl, H_RULES, 0,
           "Which words send a track to DX, FX, MX or BG, and a REAPER track template with your stems and routing.");
  }
  optRow(st, g, px, y, "Show positions in samples too", st->showSamples, true, H_SAMPLES,
         "The selected event's position and length also in samples (at the AAF's sample rate), for checking sync.");
  // clip gain: three ways
  {
    double sy = y + 4, sl = px + 96, sr = px + kOpW - 16;
    g.text(st->fBody, kInk, "Clip gain", px + 16, y, sl, y + 32, DT_LEFT);
    const char *lab[3] = {"On the items", "On the track", "Off"};
    int cur = !opt.clipGain ? 2 : (opt.clipGainToTrack ? 1 : 0);
    int ids[3] = {H_GAIN_ITEMS, H_GAIN_TRACK, H_GAIN_OFF};
    const char *tips[3] = {"Clip gain on the items, clip automation as take envelopes.",
                           "Clip gain folded into the track's volume envelope.", "No clip gain."};
    g.rbox(sl, sy, sr, sy + 26, 8, kLine, kPanel);
    double sw = (sr - sl) / 3;
    for (int i = 0; i < 3; i++) {
      double a = sl + i * sw, b2 = a + sw;
      if (i == cur) g.rfill(a + 1, sy + 1, b2 - 1, sy + 25, 7, kBlue);
      else if (i > 0 && i - 1 != cur) g.fill(a, sy + 5, a + 1, sy + 21, kLine);
      g.text(st->fSmall, i == cur ? kWhite : kInk, lab[i], a, sy, b2, sy + 26, DT_CENTER);
      addHit(st, a, sy, b2, sy + 26, ids[i], 0, tips[i]);
    }
    y += 34;
  }
  // video
  optHead(st, g, px, y, "Video");
  bool videoOk = !s.video.empty();
  bool videoFound = videoOk && fileExists(s.video[0].foundPath);
  if (videoFound) {
    bool manual = s.video[0].manual;
    std::string vl = "Video: " + baseName(s.video[0].foundPath);
    double save = y;
    optRow(st, g, px, y, vl, opt.video, true, H_VIDEO,
           manual ? "The picture you chose, on a VIDEO track at the top, at its own timecode (or where the AAF starts)."
                  : "The picture the AAF points to, on a VIDEO track at the top.");
    button(st, g, px + kOpW - 96, save + 1, px + kOpW - 16, save + 27, "Change\xe2\x80\xa6", H_VIDEO_CHOOSE, 0,
           "Choose another video file.");
  } else {
    static std::string vTip;
    vTip = videoOk ? "The AAF points to \"" + s.video[0].name +
                         "\", which isn't found. Choose the file: it goes on a VIDEO track at the top, where the AAF puts it."
                   : "Choose the picture: it goes on a VIDEO track at the top, at its own timecode (or where the AAF starts).";
    g.text(st->fBody, kMute, videoOk ? "The AAF's video isn't found." : "This AAF has no video.", px + 16, y, px + 220, y + 28,
           DT_LEFT);
    button(st, g, px + kOpW - 150, y + 1, px + kOpW - 16, y + 27, "Search for video\xe2\x80\xa6", H_VIDEO_CHOOSE, 0, vTip.c_str());
    y += 28;
  }
  // audio
  optHead(st, g, px, y, "Audio files");
  optRow(st, g, px, y, "Copy audio into a subfolder", opt.copyToSubdir, true, opt.copyToSubdir ? H_COPY_NO : H_COPY_YES,
         "On: WAV files are copied into the audio subfolder. Off: they stay where they are. MXF, AIFF and embedded audio "
         "always become Broadcast WAV in the subfolder.");
  {
    double bl = px + 16, br = px + kOpW - 104, bt = y + 4, bb = y + 44;
    std::string sub = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
    std::string dest = opt.destDir;
    if (!dest.empty() && dest.back() != '/' && dest.back() != '\\') dest += "/";
    g.rbox(bl, bt, br, bb, 8, kLine, kPanel);
    g.text(st->fSmall, kMute, opt.copyToSubdir ? "Audio goes to" : "Converted audio goes to", bl + 8, bt + 3, br - 8, bt + 19,
           DT_LEFT);
    size_t maxc = (size_t)std::max(24.0, (br - bl - 16) / 6.2);
    std::string subShown = sub.size() > maxc / 2 ? utf8Head(sub, maxc / 2 - 1) + "\xe2\x80\xa6" : sub;
    std::string shown = shortPath(dest, std::max<size_t>(12, maxc - subShown.size())) + subShown;
    if (st->editKind != 3) g.text(st->fSmall, kInk, shown, bl + 8, bt + 19, br - 8, bb - 3, DT_LEFT);
    st->subRc = devRect(st, bl + 6, bt + 19, br - 6, bb - 3);
    addHit(st, bl, bt, br, bb, H_SUBDIR, 0, "Click to rename the audio subfolder.");
    button(st, g, br + 8, bt + 5, px + kOpW - 16, bb - 5, "Choose\xe2\x80\xa6", H_DEST_CHOOSE, 0,
           "Choose the folder the audio subfolder goes into.");
    y += 52;
  }
  int ok = 0, total = 0;
  mediaCount(st, &ok, &total);
  g.text(st->fSmall, ok == total ? theme().green : kRed,
         ok == total ? strprintf("All %d media files found", total) : strprintf("%d of %d media files missing", total - ok, total),
         px + 16, y, px + 220, y + 24, DT_LEFT);
  const char *ll = "Search for files\xe2\x80\xa6";
  double lw = g.width(st->fSmall, ll);
  g.text(st->fSmall, kBlue, ll, px + kOpW - 22 - lw, y, px + kOpW - 16, y + 24, DT_RIGHT);
  addHit(st, px + kOpW - 20 - lw, y, px + kOpW - 12, y + 24, H_LOCATE, 0,
         "Choose a folder that holds media. All its subfolders are searched, and OLAAF remembers it for the next import.");
  y += 28;
  g.fill(px + 16, y, px + kOpW - 16, y + 1, kLine);
  g.text(st->fTag, kMute, "V " OLAAF_VERSION_STRING " beta developed with AI assistance \xc2\xb7 dmajorsound.com", px + 16, y + 4, px + kOpW - 16,
         y + 24, DT_LEFT);
}

// ------------------------------------------------------------------ folder sorting and track template

void loadTemplateList(State *st) {
  const std::string &p = st->opt->trackTemplate;
  if (p == st->tplLoaded) return;
  st->tplLoaded = p;
  st->tplTracks.clear();
  if (!p.empty()) olaaf::readTrackTemplate(p, &st->tplTracks, nullptr);
}

void saveRules(State *st, const std::vector<SortRule> &rules) {
  setSortRules(rules);
  bool isDefault = sortRulesToText(rules) == sortRulesToText(defaultSortRules());
  if (st->host && st->host->setSetting) st->host->setSetting("rules", isDefault ? "" : sortRulesToText(rules));
}

void paintRules(State *st, const Gfx &g) {
  loadTemplateList(st);
  const std::vector<SortRule> &rules = sortRules();
  std::vector<Folder> asFolders;
  for (auto &r : rules) asFolders.push_back({r.name, r.name, r.color, ""});
  std::map<std::string, int> match = olaaf::matchTemplate(st->tplTracks, asFolders);
  const double w = 520, rowH = 50;
  double h = 60 + rules.size() * rowH + 16 + 52 + 62 + 32 + 18;
  double l = std::min(st->optsBtn.r - 60, kR - w), t = kTopH - 6;
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 12, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 12, kLine, kPop);
  addHit(st, l, t, l + w, t + h, H_RULES_BG, 0, nullptr);
  g.text(st->fBold, kInk, "Sorting tracks into folders", l + 16, t + 12, l + w - 60, t + 32, DT_LEFT);
  g.text(st->fSmall, kMute, "A track goes into the folder when its name holds one of the words.", l + 16, t + 32, l + w - 60,
         t + 50, DT_LEFT);
  button(st, g, l + w - 44, t + 12, l + w - 16, t + 40, "\xc3\x97", H_RULES_CLOSE, 0, "Close (Esc).");
  double y = t + 60;
  const double nameR = l + 100, wordsL = l + 108, wordsR = l + w - (st->tplTracks.empty() ? 16 : 150);
  for (size_t i = 0; i < rules.size(); i++) {
    const SortRule &r = rules[i];
    double cy = y + rowH / 2 - 4;
    g.rfill(l + 16, cy - 8, l + 32, cy + 8, 8, rgbToRef(r.color));
    addHit(st, l + 12, cy - 12, l + 36, cy + 12, H_RULE_COLOR, (int)i, "The folder's color. Click to choose another one.");
    if (!(st->editKind == 6 && st->editKey == std::to_string(i)))
      g.text(st->fBold, kInk, r.name, l + 42, y, nameR, y + rowH - 8, DT_LEFT);
    addHit(st, l + 40, y + 4, nameR, y + rowH - 12, H_RULE_NAME, (int)i, "The folder's name. Click and type to rename it.");
    std::string words;
    for (size_t k = 0; k < r.words.size(); k++) words += (k ? ", " : "") + r.words[k];
    if (words.empty()) words = "(no words: nothing goes here by itself)";
    bool cut = false;
    bool typing = (st->editKind == 5) && st->editKey == std::to_string(i);
    g.rbox(wordsL, y + 2, wordsR, y + rowH - 10, 8, typing ? kBlue : kLine, kPanel);
    std::vector<std::string> wl = fitLines(g, st->fSmall, words, wordsR - wordsL - 16, 2, &cut);
    if (!typing)
      for (size_t k = 0; k < wl.size(); k++)
        g.text(st->fSmall, r.words.empty() ? kMute : kInk, wl[k], wordsL + 8, y + 5 + k * 16, wordsR - 8,
               y + 21 + k * 16, DT_LEFT);
    static std::string wTip[16];
    if (i < 16) wTip[i] = "Click and type the words for " + r.name + ", separated by commas (Return keeps them, Tab: the next folder): " + words;
    addHit(st, wordsL, y + 2, wordsR, y + rowH - 10, H_RULE_WORDS, (int)i, i < 16 ? wTip[i].c_str() : nullptr);
    if (!st->tplTracks.empty()) {
      auto m = match.find(r.name);
      std::string to = m == match.end() ? "own folder" : "\xe2\x86\x92 " + st->tplTracks[(size_t)m->second].name;
      std::vector<std::string> tl = fitLines(g, st->fSmall, to, 124, 1, &cut);
      g.text(st->fSmall, m == match.end() ? kMute : kInk, tl.empty() ? to : tl[0], wordsR + 10, y, l + w - 16, y + rowH - 8,
             DT_LEFT);
    }
    y += rowH;
  }
  g.text(st->fSmall, kMute, "Everything else goes to OTHER.", l + 16, y - 6, l + 300, y + 12, DT_LEFT);
  y += 16;
  button(st, g, l + 16, y, l + 186, y + 28, "Sort the tracks now", H_RULES_SORT, 0,
         "Puts this AAF's tracks into the folders by these words again (Cmd/Ctrl+Z undoes it).");
  button(st, g, l + 196, y, l + 336, y + 28, "Reset the words", H_RULES_RESET, 0,
         "Back to OLAAF's own folders, colors and words (DX, FX, MX, BG).");
  y += 42;
  g.fill(l + 16, y, l + w - 16, y + 1, kLine);
  y += 10;
  g.text(st->fBold, kInk, "Track template", l + 16, y, l + w - 16, y + 20, DT_LEFT);
  g.text(st->fSmall, kMute, "A REAPER track template (.RTrackTemplate) with your stems and routing comes in first.",
         l + 16, y + 20, l + w - 16, y + 38, DT_LEFT);
  g.text(st->fSmall, kMute, "Each folder goes into its template track: \"Dialog Stem\" takes DX.", l + 16, y + 36,
         l + w - 16, y + 54, DT_LEFT);
  y += 62;
  const std::string &tp = st->opt->trackTemplate;
  bool bad = !tp.empty() && st->tplTracks.empty();
  double fr = l + w - (tp.empty() ? 126 : 212);
  g.rbox(l + 16, y, fr, y + 32, 8, bad ? kRed : kLine, kPanel);
  std::string shown = tp.empty() ? "None: OLAAF makes the folders"
                                 : bad ? baseName(tp) + " (no tracks in it)"
                                       : strprintf("%s  \xc2\xb7  %zu tracks", stemName(tp).c_str(), st->tplTracks.size());
  bool cut = false;
  std::vector<std::string> sl = fitLines(g, st->fBody, shown, fr - l - 40, 1, &cut);
  g.text(st->fBody, tp.empty() ? kMute : (bad ? kRed : kInk), sl.empty() ? shown : sl[0], l + 26, y, fr - 8, y + 32, DT_LEFT);
  static std::string tTip;
  tTip = tp.empty() ? "No track template." : tp;
  addHit(st, l + 16, y, fr, y + 32, H_RULES_BG, 0, tTip.c_str());
  button(st, g, fr + 8, y + 2, fr + 110, y + 30, "Choose\xe2\x80\xa6", H_TPL_CHOOSE, 0,
         "Choose a .RTrackTemplate file. OLAAF remembers it for the next imports.");
  if (!tp.empty())
    button(st, g, fr + 118, y + 2, l + w - 16, y + 30, "Remove", H_TPL_CLEAR, 0, "Import without a track template.");
}

// ------------------------------------------------------------------ missing media

// media the import needs that are not there (embedded audio is always there)
std::vector<int> missingMedia(State *st) {
  std::vector<int> out;
  for (int mi : usedMedia(st))
    if (!mediaThere(st, mi)) out.push_back(mi);
  return out;
}

const size_t kMissRows = 9; // missing files shown at once; the wheel or the bar scrolls the rest

void paintMissing(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  std::vector<int> miss = missingMedia(st);
  if (miss.empty()) {
    st->missingOpen = false;
    return;
  }
  const size_t shown = std::min<size_t>(miss.size(), kMissRows);
  const bool scrolls = miss.size() > shown;
  int maxScroll = (int)(miss.size() - shown);
  st->missScroll = std::max(0, std::min(st->missScroll, maxScroll));
  double w = 560, rowH = 40, h = 64 + shown * rowH + (scrolls ? 22 : 0) + 52;
  double l = st->missingX > 0 ? st->missingX : kL, t = kTopH - 6;
  if (l + w > kR) l = kR - w;
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 12, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 12, kLine, kPop);
  addHit(st, l, t, l + w, t + h, H_MISS_BG, 0, nullptr);
  g.text(st->fBold, kInk, strprintf("%zu media file%s missing", miss.size(), miss.size() == 1 ? "" : "s"), l + 16, t + 12,
         l + w - 16, t + 32, DT_LEFT);
  g.text(st->fSmall, kMute, "Locate one file and OLAAF also looks for the others in its folder.", l + 16, t + 32, l + w - 60,
         t + 50, DT_LEFT);
  button(st, g, l + w - 44, t + 12, l + w - 16, t + 40, "\xc3\x97", H_MISS_CLOSE, 0, "Close (Esc).");
  double y = t + 58, listTop = y;
  const double rowR = l + w - (scrolls ? 30 : 16); // room for the scroll bar
  for (size_t k = 0; k < shown; k++) {
    size_t i = (size_t)st->missScroll + k;
    const Media &m = s.media[(size_t)miss[i]];
    std::string where = uriToPath(m.originalUri); // "Avid%20MediaFiles" -> "Avid MediaFiles"
    g.text(st->fBody, kInk, m.name.empty() ? baseName(where) : m.name, l + 16, y + 2, rowR - 94, y + 20, DT_LEFT);
    g.text(st->fSmall, kMute, shortPath(where, 68), l + 16, y + 20, rowR - 94, y + 36, DT_LEFT);
    button(st, g, rowR - 84, y + 6, rowR, y + 32, "Locate\xe2\x80\xa6", H_MISS_PICK, miss[i],
           "Choose this file. Its folder is searched for the other missing files too.");
    y += rowH;
    if (k + 1 < shown) g.fill(l + 16, y - 1, rowR, y, mixColor(kPop, kInk, 0.08));
  }
  if (scrolls) {
    // the bar: click above or below the thumb to page, the wheel scrolls
    double bx0 = l + w - 20, bx1 = l + w - 12, by0 = listTop + 4, by1 = y - 4;
    g.rfill(bx0, by0, bx1, by1, 4, mixColor(kPop, kInk, 0.08));
    double th = std::max(24.0, (by1 - by0) * shown / miss.size());
    double ty = by0 + (by1 - by0 - th) * st->missScroll / std::max(1, maxScroll);
    g.rfill(bx0, ty, bx1, ty + th, 4, theme().thumb);
    addHit(st, bx0 - 4, by0, bx1 + 4, ty, H_MISS_PAGE, -1, "Scroll up (or use the mouse wheel).");
    addHit(st, bx0 - 4, ty + th, bx1 + 4, by1, H_MISS_PAGE, 1, "Scroll down (or use the mouse wheel).");
    g.text(st->fSmall, kMute,
           strprintf("%d\xe2\x80\x93%d of %zu \xc2\xb7 scroll for the others", st->missScroll + 1,
                     st->missScroll + (int)shown, miss.size()),
           l + 16, y, l + w - 16, y + 20, DT_LEFT);
    y += 22;
  }
  g.fill(l + 16, y + 6, l + w - 16, y + 7, kLine);
  button(st, g, l + 16, y + 16, l + 196, y + 42, "Search for files\xe2\x80\xa6", H_LOCATE, 0,
         "Choose a folder that holds media. All its subfolders are searched, and OLAAF remembers it for the next import.");
  g.text(st->fSmall, kMute, "Missing files come in as offline items.", l + 210, y + 16, l + w - 16, y + 42, DT_LEFT);
}

// ------------------------------------------------------------------ help and shortcuts

void keyCap(State *st, const Gfx &g, double &x, double y, const std::string &k) {
  double w = std::max(24.0, g.width(st->fKey, k) + 14);
  g.rbox(x, y, x + w, y + 22, 5, kLine, mixColor(kPanel, kInk, 0.08));
  g.text(st->fKey, kInk, k, x, y, x + w, y + 22, DT_CENTER);
  x += w + 4;
}

void paintHelp(State *st, const Gfx &g) {
  // three blocks left, one right; drawn over everything (footer too), always inside the window
  const double rowH = 26, w = 860, h = 74 + (24 + 3 * rowH + 10) + (24 + 6 * rowH + 10) + (24 + 8 * rowH + 10) + 52;
  double l = (W - w) / 2, t = std::max(8.0, std::min(kTopH + 6, H - h - 8));
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 16, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 16, kLine, kPop);
  addHit(st, l, t, l + w, t + h, H_HELP_BG, 0, nullptr);
  g.text(st->fHead, kInk, "Help and keyboard shortcuts", l + 24, t + 16, l + w - 200, t + 40, DT_LEFT);
  g.text(st->fSmall, kMute, "The same keys on Mac and Windows. Hover over anything in OLAAF for a short hint.", l + 24, t + 40,
         l + w - 60, t + 58, DT_LEFT);
  button(st, g, l + w - 44, t + 14, l + w - 16, t + 42, "\xc3\x97", H_HELP_CLOSE, 0, "Close (Esc).");
  { // the manual on dmajorsound.com, in the browser
    const char *ml = "Read manual";
    double mw = g.width(st->fBody, ml) + 28;
    button(st, g, l + w - 52 - mw, t + 14, l + w - 52, t + 42, ml, H_HELP_MANUAL, 0,
           "Open the OLAAF manual on dmajorsound.com in your browser.");
  }

  struct KeyRow { const char *keys; const char *what; };
  // keys separated by '|', several ways by '/'
  const KeyRow play[] = {{"Space", "Play / stop from the playhead"},
                      {"Shift|Space", "Play / stop the selected event"},
                      {"Click ruler", "Move the playhead"}};
  const KeyRow view[] = {{"+/\xe2\x88\x92/T/R", "Zoom in / out (T in, R out)"},
                      {"Alt|Wheel", "Zoom (Option on the Mac)"},
                      {"Shift|Wheel", "Move through time"},
                      {"F", "Show the whole AAF"},
                      {"Z", "Zoom to the selection, playhead to it"},
                      {"A", "All automation on / off"}};
  const KeyRow ev[] = {{"V/P", "Volume / pan of a track (selected or pointed)"},
                    {"Alt|V/P", "Volume / pan of every track"},
                    {"Click", "Select an event (Shift-click: more)"},
                    {"Drag", "Select in a frame (start on an empty lane)"},
                    {"Cmd/Ctrl|A", "Select all events"},
                    {"\xe2\x86\x91/\xe2\x86\x93", "Move the selection one track up / down"},
                    {"M", "Mute / unmute"},
                    {"G", "Stereo meter for a stereo event"},
                    {"Shift|S", "Solo the selected event's track"},
                    {"Delete/Backspace", "Remove / restore"},
                    {"Drag names", "Order tracks, put them in folders"},
                    {"Double-click", "Rename (Tab: the next track\xe2\x80\x99s name)"},
                    {"C", "Two selected mono tracks into stereo"},
                    {"Shift|F", "Show the event\xe2\x80\x99s file (Finder / Explorer)"},
                    {"Shift|C", "Copy the event\xe2\x80\x99s file path"}};
  const KeyRow win[] = {{"Cmd/Ctrl|Z", "Undo (Shift: redo)"},
                     {"Return", "Import"},
                     {"O", "Options"},
                     {"H", "This help"},
                     {"1/2/3", "Show reel 1, 2, 3\xe2\x80\xa6"},
                     {"Shift|\xe2\x86\x90/\xe2\x86\x92", "Previous / next reel"},
                     {"Tab", "Next track (Shift + Tab: previous)"},
                     {"Esc", "Close a panel, then the window"}};
  auto block = [&](double x, double &y, const char *head, const KeyRow *rows, int n) {
    g.text(st->fSmall, kMute, head, x, y, x + 380, y + 20, DT_LEFT);
    y += 24;
    for (int i = 0; i < n; i++) {
      double kx = x;
      std::string keys = rows[i].keys;
      size_t a = 0;
      while (a <= keys.size()) {
        size_t b = keys.find_first_of("|/", a);
        std::string k = keys.substr(a, b == std::string::npos ? std::string::npos : b - a);
        keyCap(st, g, kx, y + 2, k);
        if (b == std::string::npos) break;
        if (keys[b] == '|') {
          g.text(st->fSmall, kMute, "+", kx - 2, y, kx + 8, y + 26, DT_CENTER);
          kx += 10;
        } else
          kx += 2;
        a = b + 1;
      }
      g.text(st->fBody, kInk, rows[i].what, x + 150, y, x + 412, y + 26, DT_LEFT);
      y += rowH;
    }
    y += 10;
  };
  double y1 = t + 74, y2 = t + 74;
  block(l + 24, y1, "Listening", play, 3);
  block(l + 24, y1, "View", view, (int)(sizeof(view) / sizeof(view[0])));
  block(l + 24, y1, "Window", win, 8);
  block(l + 424, y2, "Events and tracks", ev, (int)(sizeof(ev) / sizeof(ev[0])));
  double y = std::max(y1, y2) + 2;
  g.fill(l + 24, y, l + w - 24, y + 1, kLine);
  g.text(st->fSmall, kMute,
         "Set where the AAF starts in REAPER on top, choose what comes along under Options, then Import. "
         "Clip gain, fades, folders and video",
         l + 24, y + 8, l + w - 24, y + 26, DT_LEFT);
  g.text(st->fSmall, kMute, "come into REAPER as you see them here. V " OLAAF_VERSION_STRING " beta \xc2\xb7 dmajorsound.com", l + 24, y + 26,
         l + w - 24, y + 44, DT_LEFT);
}

// The footer: what the thing under the mouse does (or a general hint), dmajorsound on the right.
void paintFooter(State *st, const Gfx &g) {
  double t = H - kFootH, b = H;
  g.fill(0, t, W, t + 1, kLine);
  drawDmajorLogo(g.dc, g.X(kR), g.Y((t + b) / 2), g.S(16), kMute, kMute, st->fSmall);
  const char *hint = st->tip ? st->tip
                             : "Hover over anything for a hint. Space plays from the playhead, Shift + Space the selected event, H "
                               "shows all shortcuts.";
  double rr = paintUpdateNote(st, g, kR - 160, t, b); // a newer OLAAF, left of dmajorsound.com
  if (rr < kR - 160) rr -= 20;
  double hr = rr;
  // every AAF (tab) can go back to how it came in: after the last import's choices, or after edits here
  if ((st->choicesBack || st->edited || !st->undo.empty()) && !st->freshNow) {
    const char *ll = "Start fresh";
    double lw = g.width(st->fSmall, ll);
    double lr = rr;
    g.text(st->fSmall, kBlue, ll, lr - lw, t + 1, lr, b, DT_RIGHT);
    addHit(st, lr - lw - 4, t + 4, lr + 4, b - 4, H_FRESH, 0,
           reelCount(st) > 1 ? "Only this tab: back to its AAF as it is, without the changes and the last import's choices (Cmd/Ctrl+Z brings them back)."
                             : "Back to the AAF as it is, without the changes and the last import's choices (Cmd/Ctrl+Z brings them back).");
    hr = lr - lw - 20;
    if (!st->tip && st->choicesBack) hint = "Your choices from the last import of this AAF are back: names, folders, colors, removed and moved events.";
  }
  bool taken = st->tip && strstr(st->tip, " is taken by ") && strstr(st->tip, "nothing mov");
  if (!st->peaks.busy() && st->peaks.failed() > 0) { // some files found, but no waveform from them
    int nf = st->peaks.failed();
    std::string pl = strprintf("%d waveform%s couldn't be read \xc2\xb7 right-click an event for why", nf, nf == 1 ? "" : "s");
    double pw = g.width(st->fSmall, pl);
    g.text(st->fSmall, kWarn, pl, hr - pw, t + 1, hr, b, DT_RIGHT);
    hr -= pw + 20;
  }
  // background work, with a small bar: unpacking the AAF's own audio, then reading the waveforms
  auto progress = [&](const char *what, int dn, int tot) {
    std::string pl = strprintf("%s\xe2\x80\xa6 %d of %d", what, dn, tot);
    double pw = g.width(st->fSmall, pl), barW = 90, bx = hr - barW, cy = (t + b) / 2;
    g.rfill(bx, cy - 3, hr, cy + 3, 3, mixColor(kPanel, kInk, 0.12));
    g.rfill(bx, cy - 3, bx + std::max(6.0, barW * dn / tot), cy + 3, 3, kBlue);
    g.text(st->fSmall, kInk, pl, bx - 10 - pw, t + 1, bx - 8, b, DT_RIGHT);
    hr = bx - 24 - pw;
  };
  if (st->embedJob && !st->embedJob->media.empty()) {
    int tot = (int)st->embedJob->media.size();
    progress("Unpacking embedded audio", std::min(st->embedJob->done.load(), tot), tot);
  } else if (st->peaks.busy() && st->peaks.total() > 0)
    progress("Creating waveforms", std::min(st->peaks.done(), st->peaks.total()), st->peaks.total());
  g.text(st->fSmall, taken ? kRed : (st->tip ? kInk : kMute), hint, kL, t + 1, hr, b, DT_LEFT);
}

// ------------------------------------------------------------------ frame rate warning

// ------------------------------------------------------------------ embedded audio

static void embedStartNext(State *st) {
  if (st->embedJob || st->embedQueue.empty()) return;
  auto next = st->embedQueue.front();
  st->embedQueue.erase(st->embedQueue.begin());
  std::unique_ptr<State::EmbedJob> j(new State::EmbedJob);
  j->imp = next.first;
  j->dir = next.second;
  const Session &s = j->imp->session();
  for (size_t i = 0; i < s.media.size(); i++)
    if (s.media[i].kind == MediaKind::Embedded && s.media[i].foundPath.empty()) j->media.push_back((int)i);
  State::EmbedJob *jp = j.get();
  j->th = std::thread([jp]() {
    for (int mi : jp->media) {
      if (jp->stop) break;
      std::string p = jp->imp->extractEmbeddedPreview(mi, jp->dir);
      {
        std::lock_guard<std::mutex> l(jp->mu);
        jp->ready.push_back({mi, p});
      }
      jp->done++;
    }
    jp->finished = true;
  });
  st->embedJob = std::move(j);
  if (st->hwnd) SetTimer(st->hwnd, kPeakTimer, 150, NULL);
}

// the window's side: unpacked files go into the session; a finished AAF gets its waveforms
static void embedPoll(State *st) {
  State::EmbedJob *j = st->embedJob.get();
  if (!j) return;
  std::vector<std::pair<int, std::string>> got;
  {
    std::lock_guard<std::mutex> l(j->mu);
    got.swap(j->ready);
  }
  Session &s = j->imp->session();
  for (auto &g : got)
    if (!g.second.empty() && g.first >= 0 && g.first < (int)s.media.size()) s.media[(size_t)g.first].foundPath = g.second;
  if (!got.empty()) st->mediaRev++;
  if (!j->finished) return;
  { // (the thread may have added its last file after the swap above)
    std::lock_guard<std::mutex> l(j->mu);
    for (auto &g : j->ready)
      if (!g.second.empty() && g.first >= 0 && g.first < (int)s.media.size()) s.media[(size_t)g.first].foundPath = g.second;
    j->ready.clear();
  }
  if (j->th.joinable()) j->th.join();
  if (!j->stop) j->imp->saveEmbeddedCache(j->dir);
  bool shown = j->imp == st->imp;
  st->embedJob.reset();
  if (shown) {
    st->peaks.start(st->imp->session());
    st->peakSeen = -1;
  }
  embedStartNext(st);
}

// stops unpacking (one AAF, or all with nullptr) and waits for the file in work
static void embedCancel(State *st, Importer *imp) {
  for (size_t i = st->embedQueue.size(); i-- > 0;)
    if (!imp || st->embedQueue[i].first == imp) st->embedQueue.erase(st->embedQueue.begin() + (long)i);
  if (st->embedJob && (!imp || st->embedJob->imp == imp)) {
    st->embedJob->stop = true;
    if (st->embedJob->th.joinable()) st->embedJob->th.join();
    st->embedJob->finished = true;
    embedPoll(st); // what is unpacked so far is kept (the import copies it)
  }
  if (imp) st->embedAsked.erase(imp);
}

// the shown AAF has audio inside: use an earlier unpacking, or ask (once per AAF)
static void embedCheck(State *st) {
  if (!st->host || !st->host->embeddedDir || st->embedAsked.count(st->imp)) return;
  if (st->imp->embeddedCount() == 0) return;
  st->embedAsked.insert(st->imp);
  st->mediaRev++;
  if (st->imp->loadEmbeddedCache(st->host->embeddedDir(st->imp->session().aafPath))) {
    st->peaks.start(st->imp->session());
    st->peakSeen = -1;
    return;
  }
  st->embedAsk = true;
}

static void embedAnswer(State *st, bool load) {
  st->embedAsk = false;
  if (load && st->host && st->host->embeddedDir) {
    st->embedQueue.push_back({st->imp, st->host->embeddedDir(st->imp->session().aafPath)});
    embedStartNext(st);
    st->tip = "Unpacking the AAF's audio in the background: waveforms and listening follow when it is done.";
  } else
    st->tip = "Items only: the AAF's audio is unpacked on import.";
}

// A question the user answers before going on: title, what was found, gray notes, two buttons
// (right-aligned on their own row; the suggested one is blue). The height follows the text.
struct AskButton { const char *label; int id; const char *tip; bool primary; };
void paintAsk(State *st, const Gfx &g, const char *title, const std::string &finding,
              const std::vector<const char *> &noteTexts, int bgHit, const AskButton &left, const AskButton &right) {
  const double w = 540, pad = 24, tw = w - 2 * pad, btnW = 150, btnH = 32, btnGap = 10;
  bool cut = false;
  std::vector<std::string> body = fitLines(g, st->fBody, finding, tw, 3, &cut);
  std::vector<std::string> notes;
  for (const char *n : noteTexts)
    for (auto &ln : fitLines(g, st->fSmall, n, tw, 3, &cut)) notes.push_back(ln);
  const double titleH = 26, bodyLH = 21, noteLH = 18;
  double h = pad + titleH + 10 + body.size() * bodyLH + 10 + notes.size() * noteLH + 20 + btnH + pad;
  double cx = (kTlx + kTlr) / 2 - 40, cy = (kLT + kLB) / 2;
  double l = cx - w / 2, t = cy - h / 2;
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 14, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 14, kLine, kPop);
  addHit(st, l, t, l + w, t + h, bgHit, 0, nullptr);
  double y = t + pad;
  g.text(st->fHead, kInk, title, l + pad, y, l + w - pad, y + titleH, DT_LEFT);
  y += titleH + 10;
  for (auto &ln : body) {
    g.text(st->fBody, kInk, ln, l + pad, y, l + w - pad, y + bodyLH, DT_LEFT);
    y += bodyLH;
  }
  y += 10;
  for (auto &ln : notes) {
    g.text(st->fSmall, kMute, ln, l + pad, y, l + w - pad, y + noteLH, DT_LEFT);
    y += noteLH;
  }
  double bb = t + h - pad, bt = bb - btnH, br = l + w - pad;
  button(st, g, br - btnW, bt, br, bb, right.label, right.id, 0, right.tip, right.primary);
  button(st, g, br - 2 * btnW - btnGap, bt, br - btnW - btnGap, bb, left.label, left.id, 0, left.tip, left.primary);
}

void paintEmbedAsk(State *st, const Gfx &g) {
  if (!st->embedAsk || st->reelAsk) return;
  uint64_t bytes = 0;
  int n = st->imp->embeddedCount(&bytes);
  std::string size = bytes >= (1ull << 30) ? strprintf("about %.1f GB", bytes / 1073741824.0)
                                           : strprintf("about %d MB", (int)std::max<uint64_t>(1, bytes >> 20));
  paintAsk(st, g, "Embedded audio",
           strprintf("This AAF carries its audio inside the file: %d file%s, %s.", n, n == 1 ? "" : "s", size.c_str()),
           {"Load waveforms: OLAAF unpacks the audio into a temporary folder first, so you see waveforms and can listen. "
            "This preview takes a little longer to get ready, more so for big AAFs.",
            "Items only: nothing is unpacked now. You see the events without waveforms; the audio is unpacked on import."},
           H_EMBED_BG,
           {"Load waveforms", H_EMBED_LOAD, "Unpack the audio now (in the background) for waveforms and listening. Takes a little longer.", true},
           {"Items only", H_EMBED_SKIP, "Show the events without waveforms. Nothing is unpacked until the import.", false});
}

// several AAFs: do they look like one film? Then ask whether they share tracks.
void askAboutReels(State *st) {
  // one film (numbered reels, the same track layout)? Then ask: same tracks or their own?
  std::vector<Session> ss;
  std::vector<std::string> names;
  for (int i : shownReels(st)) {
    Importer *ri;
    ImportOptions *ro;
    if (!reelAt(st, i, &ri, &ro)) continue;
    ss.push_back(ri->session());
    names.push_back(reelLabel(ri->session(), i));
  }
  if (ss.size() >= 2) {
    std::vector<std::string> nl = reelNumberLabels(names);
    if (nl.empty()) nl = reelShortLabels(names);
    st->reelAskNames.clear();
    for (size_t k = 0; k < nl.size(); k++)
      st->reelAskNames += (k == 0 ? "" : k + 1 == nl.size() ? " and " : ", ") + nl[k];
    st->reelAskRelated = reelsBelongTogether(ss, &st->reelAskWhy);
    st->reelAsk = true;
  }
}

// same tracks for all AAFs, or each its own: one setting for every tab
void reelAnswer(State *st, bool share) {
  st->reelAsk = false;
  for (int i = 0; i < reelCount(st); i++) {
    Importer *ri;
    ImportOptions *ro;
    if (reelAt(st, i, &ri, &ro)) ro->reelsShareTracks = share;
  }
  st->opt->reelsShareTracks = share;
  st->tip = share ? "Same tracks: tracks with the same name share one REAPER track."
                  : "Own tracks: every AAF gets its own REAPER tracks.";
}

// after adding AAFs: do they go onto the same tracks, or their own?
void paintReelAsk(State *st, const Gfx &g) {
  if (!st->reelAsk) return;
  bool rec = st->reelAskRelated; // the suggested answer is blue
  paintAsk(st, g, rec ? "Same film?" : "A different film?",
           rec ? st->reelAskNames + " look like parts of one film: " + st->reelAskWhy + "."
               : st->reelAskNames + " have different track layouts.",
           {"Same tracks: tracks with the same name share one REAPER track.",
            "Own tracks: every AAF keeps its own tracks. Automation never mixes either way.",
            "You can change it any time next to the tabs."},
           H_REEL_ASK_BG,
           {"Same tracks", H_REEL_SHARE, "Tracks with the same name share one REAPER track; each AAF's automation stays in its own time.", rec},
           {"Own tracks", H_REEL_OWN, "Every AAF gets its own REAPER tracks, with the reel in the name.", !rec});
}

void paintFpsWarning(State *st, const Gfx &g) {
  double proj = 0, projSr = 0;
  bool fps = fpsMismatch(st, &proj), sr = srMismatch(st, &projSr);
  if (st->fpsDismissed || (!fps && !sr)) return;
  Session &s = st->imp->session();
  // height from the content: title, the mismatch lines, the notes, then the button on its own row
  int bodyN = (fps ? 1 : 0) + (sr ? 1 : 0), noteN = (fps ? 1 : 0) + (sr ? 1 : 0) + 1;
  double w = 520, h = 18 + 26 + bodyN * 22 + 6 + noteN * 17 + 20 + 32 + 20;
  double cx = (kTlx + kTlr) / 2 - 40, cy = (kLT + kLB) / 2;
  double l = cx - w / 2, t = cy - h / 2;
  g.rfill(l - 1, t + 2, l + w + 3, t + h + 6, 14, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
  g.rbox(l, t, l + w, t + h, 14, kWarn, kPop);
  addHit(st, l, t, l + w, t + h, H_OPTS_BG, 0, nullptr);
  double tx = l + 44, ty = t + 46; // the triangle
  g.poly({{tx, ty - 22}, {tx + 24, ty + 18}, {tx - 24, ty + 18}}, kWarn);
  g.fill(tx - 1.6, ty - 8, tx + 1.6, ty + 6, kPop);
  g.fill(tx - 1.6, ty + 9, tx + 1.6, ty + 12.5, kPop);
  const char *title = fps && sr ? "Frame rate and sample rate don't match" : fps ? "Frame rate mismatch" : "Sample rate mismatch";
  g.text(st->fHead, kInk, title, l + 84, t + 18, l + w - 16, t + 44, DT_LEFT);
  double y = t + 48;
  if (fps) {
    g.text(st->fBody, kInk,
           strprintf("The AAF runs at %s, this REAPER project at %s fps.", fpsText(s).c_str(), fpsShort(proj).c_str()), l + 84,
           y, l + w - 16, y + 20, DT_LEFT);
    y += 22;
  }
  if (sr) {
    g.text(st->fBody, kInk,
           strprintf("The AAF is %g kHz, this REAPER project %g kHz.", s.sampleRate / 1000.0, projSr / 1000.0), l + 84, y,
           l + w - 16, y + 20, DT_LEFT);
    y += 22;
  }
  y += 6;
  if (fps) {
    g.text(st->fSmall, kMute, "Frame rate: timecode and frame edges won't line up.", l + 84, y, l + w - 16, y + 18, DT_LEFT);
    y += 17;
  }
  if (sr) {
    g.text(st->fSmall, kMute, "Sample rate: REAPER resamples on playback, which can cost quality.", l + 84,
           y, l + w - 16, y + 18, DT_LEFT);
    y += 17;
  }
  g.text(st->fSmall, kMute, "Set both in File \xe2\x80\xba Project settings, or export the AAF again.", l + 84, y, l + w - 16,
         y + 18, DT_LEFT);
  button(st, g, l + w - 144, t + h - 52, l + w - 24, t + h - 20, "Got it", H_FPS_OK, 0, "Hide this warning.", true);
}

// ------------------------------------------------------------------ popovers

const double kFpRowH = 26, kFpW = 250;

void folderPopoverRect(State *st, double *px, double *py, double *ph) {
  int n = (int)st->imp->session().folders.size();
  *ph = 30 + (n + 1) * kFpRowH + 42;
  *px = st->fpX;
  *py = st->fpY;
  if (*py + *ph > H - 20) *py = st->fpY0 - *ph;
  *py = std::max(*py, 60.0);
}

RECT folderNameRect(State *st, int i) {
  double px, py, ph;
  folderPopoverRect(st, &px, &py, &ph);
  double y = py + 30 + i * kFpRowH;
  return devRect(st, px + 28, y + 3, px + kFpW - 64, y + kFpRowH - 3);
}

void paintFolderPopover(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  if (st->folderTrack < 0 || st->folderTrack >= (int)s.tracks.size()) return;
  Track &tr = s.tracks[(size_t)st->folderTrack];
  const double rowH = kFpRowH, pw = kFpW;
  int n = (int)s.folders.size();
  double px, py, ph;
  folderPopoverRect(st, &px, &py, &ph);
  g.rbox(px, py, px + pw, py + ph, 10, kLine, kPop);
  addHit(st, px, py, px + pw, py + ph, H_FOLD_POP, 0, nullptr);
  g.text(st->fSec, kMute, "Folder \xc2\xb7 " + tr.name, px + 10, py + 4, px + pw - 10, py + 26, DT_LEFT);
  double y = py + 30;
  for (int i = 0; i < n; i++, y += rowH) {
    const Folder &f = s.folders[(size_t)i];
    bool cur = tr.group == f.key;
    const double ix = f.parent.empty() ? 0 : 14; // a sub-folder: indented under its folder
    if (cur) g.fill(px + 1, y, px + pw - 1, y + rowH, kBlueSoft);
    g.box(px + 9 + ix, y + 5, px + 25 + ix, y + 21, theme().empty, f.color ? rgbToRef(f.color) : kPanel);
    addHit(st, px + 7 + ix, y + 3, px + 27 + ix, y + 23, H_FOLD_COLOR, i, "Set the folder color.");
    bool renaming = st->editKind == 2 && st->editKey == f.key;
    if (!renaming) g.text(cur ? st->fBold : st->fBody, kInk, f.name, px + 30 + ix, y, px + pw - 64, y + rowH, DT_LEFT);
    int nTr = 0;
    for (auto &t : s.tracks) nTr += t.group == f.key;
    g.text(st->fClip, kMute, std::to_string(nTr), px + pw - 84, y, px + pw - 62, y + rowH, DT_RIGHT);
    addHit(st, px + 28, y, px + pw - 60, y + rowH, H_FOLD_ASSIGN, i, "Put the track into this folder.");
    button(st, g, px + pw - 56, y + 3, px + pw - 34, y + rowH - 3, "\xe2\x9c\x8e", H_FOLD_RENAME, i, "Rename this folder.");
    button(st, g, px + pw - 30, y + 3, px + pw - 8, y + rowH - 3, "\xc3\x97", H_FOLD_DELETE, i,
           f.parent.empty() ? "Don't create this folder. Its tracks are imported outside any folder."
                            : "Don't create this sub-folder. Its tracks go into the folder around it.");
  }
  bool none = tr.group.empty() || !s.folder(tr.group);
  if (none) g.fill(px + 1, y, px + pw - 1, y + rowH, kBlueSoft);
  g.text(none ? st->fBold : st->fBody, kMute, "No folder", px + 30, y, px + pw - 10, y + rowH, DT_LEFT);
  addHit(st, px + 1, y, px + pw - 1, y + rowH, H_FOLD_NONE, 0, "Import this track outside any folder.");
  y += rowH + 8;
  button(st, g, px + 10, y, px + 130, y + 26, "+ New folder", H_FOLD_NEW, 0, "Add a folder and put this track into it.");
}

const int kDpRows = 9;
const double kDpRowH = 24, kDpW = 260;

void paintDestPopover(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  if (st->destTrack < 0 || st->destTrack >= (int)s.tracks.size()) return;
  Track &tr = s.tracks[(size_t)st->destTrack];
  int n = (int)st->projTracks.size();
  int shown = std::min(n, kDpRows);
  double ph = 30 + 3 * kDpRowH + 9 + std::max(1, shown) * kDpRowH + 8;
  double px = std::min(st->dpX, (double)W - 16 - kDpW), py = st->dpY;
  if (py + ph > H - 20) py = st->dpY0 - ph;
  py = std::max(py, 60.0);
  g.rbox(px, py, px + kDpW, py + ph, 10, kLine, kPop);
  addHit(st, px, py, px + kDpW, py + ph, H_DEST_POP, 0, nullptr);
  g.text(st->fSec, kMute, "Put on \xc2\xb7 " + tr.name, px + 10, py + 4, px + kDpW - 10, py + 26, DT_LEFT);
  double y = py + 30;
  bool isNew = tr.target.empty();
  if (isNew) g.fill(px + 1, y, px + kDpW - 1, y + kDpRowH, kBlueSoft);
  g.text(isNew ? st->fBold : st->fBody, kInk, "A new track", px + 12, y, px + kDpW - 10, y + kDpRowH, DT_LEFT);
  addHit(st, px + 1, y, px + kDpW - 1, y + kDpRowH, H_DEST_NEW, 0, "Import this track as a new REAPER track.");
  y += kDpRowH;
  g.text(st->fBody, kBlue, "Match all tracks by name", px + 12, y, px + kDpW - 10, y + kDpRowH, DT_LEFT);
  addHit(st, px + 1, y, px + kDpW - 1, y + kDpRowH, H_DEST_MATCHALL, 0,
         "Every AAF track goes onto the REAPER track with the same name; the others stay new tracks.");
  y += kDpRowH;
  checkbox(st, g, px + 12, y + 4, st->opt->overwriteEnvelopes, H_DEST_OVERWRITE, 0,
           "On: the AAF's volume and pan automation replaces the envelopes of the existing tracks where the AAF has events. "
           "Off: their envelopes stay untouched.");
  g.text(st->fBody, kInk, "Overwrite track envelopes", px + 36, y, px + kDpW - 10, y + kDpRowH, DT_LEFT);
  y += kDpRowH + 4;
  g.fill(px + 8, y, px + kDpW - 8, y + 1, kLine);
  y += 5;
  if (n == 0) {
    g.text(st->fSmall, kMute, "Your project has no tracks yet.", px + 12, y, px + kDpW - 10, y + kDpRowH, DT_LEFT);
    return;
  }
  st->destScroll = std::max(0, std::min(st->destScroll, n - shown));
  for (int i = 0; i < shown; i++, y += kDpRowH) {
    int k = st->destScroll + i;
    bool cur = tr.target == st->projTracks[(size_t)k].first;
    if (cur) g.fill(px + 1, y, px + kDpW - 1, y + kDpRowH, kBlueSoft);
    g.text(cur ? st->fBold : st->fBody, kInk, st->projTracks[(size_t)k].second, px + 12, y, px + kDpW - 22, y + kDpRowH, DT_LEFT);
    addHit(st, px + 1, y, px + kDpW - 1, y + kDpRowH, H_DEST_PICK, k, "Put the events on this REAPER track.");
  }
  if (n > shown) // a thin bar: where in the list we are (mouse wheel scrolls)
    g.fill(px + kDpW - 6, py + 30 + 3 * kDpRowH + 9 + (double)st->destScroll / n * shown * kDpRowH, px + kDpW - 3,
           py + 30 + 3 * kDpRowH + 9 + (double)(st->destScroll + shown) / n * shown * kDpRowH, theme().thumb);
}

void paintPalette(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  uint32_t cur = 0;
  std::string title, noneLabel = "None";
  const char *noneTip = "REAPER's default color.";
  if (!st->palFolder.empty()) {
    Folder *f = s.folder(st->palFolder);
    if (!f) return;
    cur = f->color;
    title = f->name;
  } else {
    if (st->palTrack < 0 || st->palTrack >= (int)s.tracks.size()) return;
    Track &t = s.tracks[(size_t)st->palTrack];
    cur = t.color;
    title = t.name;
    if (st->opt->folders && !t.group.empty() && s.folder(t.group)) {
      noneLabel = "Folder";
      noneTip = "No color of its own: the track takes the color of its folder.";
    }
  }
  const double pw = 176, ph = 126;
  double px = std::min(st->palX, (double)W - 16 - pw);
  double py = st->palY + 4;
  if (py + ph > H - 20) py = st->palY0 - 4 - ph;
  g.rbox(px, py, px + pw, py + ph, 10, kLine, kPop);
  addHit(st, px, py, px + pw, py + ph, H_PAL_BG, 0, nullptr);
  g.text(st->fSec, kMute, "Color \xc2\xb7 " + title, px + 10, py + 4, px + 170, py + 24, DT_LEFT);
  for (int i = 0; i < 10; i++) {
    double sx = px + 10 + (i % 5) * 32, sy = py + 28 + (i / 5) * 30;
    g.box(sx, sy, sx + 24, sy + 24, cur == kPalette[i] ? kInk : RGB(0x99, 0x99, 0x99), rgbToRef(kPalette[i]));
    addHit(st, sx, sy, sx + 24, sy + 24, H_PAL, i, "Pick this color.");
  }
  button(st, g, px + 10, py + 92, px + 86, py + 116, "Custom\xe2\x80\xa6", H_PAL_CUSTOM, 0, "Any color.");
  button(st, g, px + 92, py + 92, px + 166, py + 116, noneLabel, H_PAL_NONE, 0, noneTip);
}

void paint(State *st, HDC dc) {
  Gfx g{dc, st->scale, st->ox, st->oy};
  st->hits.clear();
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  {
    HBRUSH br = CreateSolidBrush(kFace);
    FillRect(dc, &cr, br);
    DeleteObject(br);
  }
  paintTopBar(st, g);
  paintArrange(st, g);
  paintEventBar(st, g);
  paintFpsWarning(st, g);
  paintReelAsk(st, g);
  paintEmbedAsk(st, g);
  if (st->folderTrack >= 0 && st->opt->folders) paintFolderPopover(st, g);
  if (st->destTrack >= 0) paintDestPopover(st, g);
  if (st->palTrack >= 0 || !st->palFolder.empty()) paintPalette(st, g);
  if (st->optsOpen) paintOptions(st, g);
  if (st->missingOpen) paintMissing(st, g);
  if (st->rulesOpen) paintRules(st, g);
  paintFooter(st, g);
  if (st->helpOpen) paintHelp(st, g); // over the footer too
  if (st->themeMenu) { // System / Dark / Light, under the OLAAF logo
    static const char *names[3] = {"System", "Dark", "Light"};
    static const char *tips[3] = {"Follows the colors of your REAPER theme.", "OLAAF's own dark look.", "A white interface."};
    double w = 190, rh = 30, h = 3 * rh + 40;
    double l = 10, r = l + w, t = kTopH - 8, b = t + h;
    g.rfill(l - 1, t + 2, r + 3, b + 5, 10, mixColor(kFace, RGB(0, 0, 0), olaaf::darkTheme() ? 0.35 : 0.10));
    g.rbox(l, t, r, b, 10, kLine, kPop);
    addHit(st, l, t, r, b, H_THEME_BG, 0, nullptr);
    g.text(st->fSmall, kMute, "Interface", l + 14, t + 8, r - 10, t + 30, DT_LEFT);
    for (int i = 0; i < 3; i++) {
      double y = t + 32 + i * rh;
      bool cur = olaaf::themeMode() == i;
      if (cur) g.rfill(l + 6, y, r - 6, y + rh, 6, kBlueSoft);
      if (cur) g.check(l + 12, y + 7, kBlue);
      g.text(st->fBody, kInk, names[i], l + 36, y, r - 10, y + rh, DT_LEFT);
      addHit(st, l + 6, y, r - 6, y + rh, H_THEME_PICK, i, tips[i]);
    }
  }
  paintGonio(st, g);
}

// ------------------------------------------------------------------ input

const Hit *hitAt(State *st, int x, int y) {
  for (auto it = st->hits.rbegin(); it != st->hits.rend(); ++it) {
    POINT p = {x, y};
    if (PtInRect(&it->r, p)) return &*it;
  }
  return nullptr;
}

const ClipHit *clipAt(State *st, int x, int y) {
  POINT p = {x, y};
  if (!PtInRect(&st->lanesRc, p)) return nullptr;
  for (auto it = st->clipHits.rbegin(); it != st->clipHits.rend(); ++it)
    if (PtInRect(&it->r, p)) return &*it;
  return nullptr;
}

std::vector<int> selection(State *st) { return std::vector<int>(st->sel.begin(), st->sel.end()); }

void stopPlay(State *st);
void buildRows(State *st);

void pushUndo(State *st) {
  Session &s = st->imp->session();
  st->undo.push_back({s.tracks, s.folders, st->sel, st->edited});
  if (st->undo.size() > 100) st->undo.erase(st->undo.begin());
  st->redo.clear();
  st->freshNow = false;
}

void undoRedo(State *st, bool isUndo) {
  auto &from = isUndo ? st->undo : st->redo;
  auto &to = isUndo ? st->redo : st->undo;
  if (from.empty()) {
    st->tip = isUndo ? "Nothing to undo." : "Nothing to redo.";
    return;
  }
  Session &s = st->imp->session();
  to.push_back({s.tracks, s.folders, st->sel, st->edited});
  State::Snap &sn = from.back();
  s.tracks = sn.tracks;
  s.folders = sn.folders;
  st->sel = sn.sel;
  st->edited = sn.edited;
  st->freshNow = false;
  from.pop_back();
  if (st->playClip || st->playTL) stopPlay(st);
  st->selTracks.clear();
  st->folderTrack = st->destTrack = st->palTrack = -1;
  st->palFolder.clear();
  buildRows(st);
}

bool isEditHit(int id) {
  switch (id) {
  case H_TRACK_CHECK: case H_PAL: case H_PAL_CUSTOM: case H_PAL_NONE: case H_FOLD_ASSIGN: case H_FOLD_NONE:
  case H_FOLD_DELETE: case H_FOLD_NEW: case H_EV_REMOVE: case H_EV_RESTORE: case H_EV_UP: case H_EV_DOWN:
  case H_EV_RESET: case H_EV_MUTE: case H_NEW_FOLDER: case H_TRACK_DELETE: case H_DEST_NEW: case H_DEST_MATCHALL:
  case H_DEST_PICK: case H_EV_SWAP: case H_TRACK_SWAP: case H_RULES_SORT: case H_REMOVE_EMPTY: case H_TRACK_STEREO: case H_SORT_NOW: case H_SORT_RESET: case H_SORT_SUBS:
    return true;
  }
  return false;
}

void toggleMute(State *st) {
  bool all = true;
  for (int id : st->sel)
    if (Clip *k = st->imp->findClip(id)) all = all && k->mute;
  for (int id : st->sel)
    if (Clip *k = st->imp->findClip(id)) k->mute = !all;
  if (!st->sel.empty()) st->edited = true;
}

void toggleExcluded(State *st) {
  bool anyIn = false;
  for (int id : st->sel)
    if (Clip *k = st->imp->findClip(id)) anyIn = anyIn || !k->excluded;
  st->imp->setExcluded(selection(st), anyIn);
  if (!st->sel.empty()) st->edited = true;
}

// "Locate…" for one missing file: its folder is searched for the others too
void locateOne(State *st, int mi) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  if (mi < 0 || mi >= (int)s.media.size() || !st->host || !st->host->pickFile) return;
  std::string p;
  std::string title = "OLAAF: locate " + s.media[(size_t)mi].name;
  if (!st->host->pickFile(title.c_str(), &p) || p.empty()) return;
  opt.searchFolders.insert(opt.searchFolders.begin(), dirName(p)); // the others may be next to it
  st->imp->locateMedia(opt);
  st->mediaRev++;
  s.media[(size_t)mi].foundPath = p; // the file chosen by hand wins
  st->peaks.start(s);
  SetTimer(st->hwnd, kPeakTimer, 150, NULL);
}

void menuAdd(HMENU m, int id, const std::string &text, bool enabled) {
  int pos = GetMenuItemCount(m);
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
  std::vector<wchar_t> w((size_t)std::max(1, n));
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
  InsertMenuW(m, (UINT)pos, MF_BYPOSITION | MF_STRING | (enabled ? 0 : MF_GRAYED), (UINT_PTR)id, w.data());
#else
  InsertMenu(m, pos, MF_BYPOSITION | MF_STRING | (enabled ? 0 : MF_GRAYED), (UINT_PTR)id, text.c_str());
  if (!enabled) EnableMenuItem(m, id, MF_BYCOMMAND | MF_GRAYED);
#endif
}

void menuSep(HMENU m) { InsertMenu(m, GetMenuItemCount(m), MF_BYPOSITION | MF_SEPARATOR, 0, NULL); }

// left and right exchanged for every stereo event of a track (or back)
void swapTrack(State *st, int idx) {
  Session &s = st->imp->session();
  if (idx < 0 || idx >= (int)s.tracks.size()) return;
  bool all = true;
  for (auto &c : s.tracks[(size_t)idx].clips)
    if (clipIsStereo(st, c)) all = all && c.swapLR;
  for (auto &c : s.tracks[(size_t)idx].clips)
    if (clipIsStereo(st, c)) c.swapLR = !all;
  st->edited = true;
  if (st->playClip || st->playTL) stopPlay(st);
}

void pushUndo(State *st);

// L+R: combines an .L / .R pair into one stereo track, or splits it again (an undo step is pushed before)
void toggleStereo(State *st, int idx) {
  Session &s = st->imp->session();
  {
    if (idx < 0 || idx >= (int)s.tracks.size()) return;
    static std::string why;
    why.clear();
    if (st->playClip || st->playTL) stopPlay(st);
    bool combined = s.tracks[(size_t)idx].channels == 2 && !s.tracks[(size_t)idx].mergedFrom.empty();
    int r = combined ? st->imp->splitStereo(idx, &why) : st->imp->combineStereo(idx, &why);
    if (r < 0) {
      if (!st->undo.empty()) st->undo.pop_back(); // nothing changed
      st->tip = why.c_str();
      return;
    }
    why = combined ? "Split into " + s.tracks[(size_t)r].name + " and " + s.tracks[(size_t)r + 1].name + "."
                   : "Combined into the stereo track " + s.tracks[(size_t)r].name + ".";
    st->tip = why.c_str();
    st->sel.clear();
    st->selTracks.clear();
    st->edited = true;
    buildRows(st); // the waveforms stay: the files are the same
  }
}

// The two selected tracks, upper one first (it becomes the left side): false unless exactly two mono
// tracks are selected that aren't combined already.
bool selectedPair(State *st, int *left, int *right) {
  Session &s = st->imp->session();
  if (st->selTracks.size() != 2) return false;
  int a = *st->selTracks.begin(), b = *st->selTracks.rbegin();
  for (int t : {a, b})
    if (t < 0 || t >= (int)s.tracks.size() || s.tracks[(size_t)t].channels != 1 || !s.tracks[(size_t)t].mergedFrom.empty())
      return false;
  int ra = -1, rb = -1; // the order on screen (folders can change it)
  for (size_t r = 0; r < st->rows.size(); r++) {
    if (st->rows[r].kind != 0) continue;
    if (st->rows[r].track == a) ra = (int)r;
    if (st->rows[r].track == b) rb = (int)r;
  }
  if (rb >= 0 && (ra < 0 || rb < ra)) std::swap(a, b);
  *left = a;
  *right = b;
  return true;
}

// C: the two selected mono tracks become one stereo track (upper = left); a selected combined track splits again
void combineSelected(State *st) {
  Session &s = st->imp->session();
  static std::string why;
  int l = -1, r = -1;
  if (selectedPair(st, &l, &r)) {
    if (st->playClip || st->playTL) stopPlay(st);
    pushUndo(st);
    why.clear();
    std::string ln = s.tracks[(size_t)l].name, rn = s.tracks[(size_t)r].name;
    int t = st->imp->combineTracks(l, r, &why);
    if (t < 0) {
      st->undo.pop_back(); // nothing changed
      st->tip = why.c_str();
      return;
    }
    why = "Combined into the stereo track " + s.tracks[(size_t)t].name + ": " + ln + " left, " + rn +
          " right. Right-click the name to swap the sides, C again splits it.";
    st->tip = why.c_str();
    st->sel.clear();
    st->selTracks.clear();
    st->selTracks.insert(t);
    st->edited = true;
    buildRows(st);
    return;
  }
  if (st->selTracks.size() == 1) { // one track: combine with its .L / .R partner, or split a combined one
    int t = *st->selTracks.begin();
    if (t >= 0 && t < (int)s.tracks.size() &&
        ((s.tracks[(size_t)t].channels == 2 && !s.tracks[(size_t)t].mergedFrom.empty()) || st->imp->stereoPartner(t) >= 0)) {
      pushUndo(st);
      toggleStereo(st, t);
      return;
    }
  }
  st->tip = "Select two mono tracks (click one name, Shift-click the other), then press C to combine them into one stereo track.";
}

void startEdit(State *st, int kind, RECT r, const std::string &text, const std::string &key);

// Right-click on a track name: swap left and right for the whole track, combine / split .L / .R
void trackMenu(State *st, POINT screen, int idx) {
  Session &s = st->imp->session();
  if (idx < 0 || idx >= (int)s.tracks.size()) return;
  const Track &tr = s.tracks[(size_t)idx];
  bool stereo = false, sw = true;
  for (auto &c : tr.clips)
    if (clipIsStereo(st, c)) {
      stereo = true;
      sw = sw && c.swapLR;
    }
  bool combined = tr.channels == 2 && !tr.mergedFrom.empty();
  int partner = combined ? -1 : st->imp->stereoPartner(idx);
  HMENU m = CreatePopupMenu();
  menuAdd(m, 1, tr.name, false);
  menuSep(m);
  if (stereo) menuAdd(m, 10, sw ? "Put left and right back (whole track)" : "Swap left and right (whole track)", true);
  if (combined) menuAdd(m, 11, "Split into " + tr.mergedFrom.substr(0, tr.mergedFrom.find(" + ")) + " and " +
                                   tr.mergedFrom.substr(tr.mergedFrom.find(" + ") + 3), true);
  if (partner >= 0) menuAdd(m, 12, "Combine with " + s.tracks[(size_t)partner].name + " into one stereo track", true);
  int pl = -1, pr = -1;
  bool pair = st->selTracks.count(idx) && selectedPair(st, &pl, &pr);
  if (pair)
    menuAdd(m, 14, "Combine " + s.tracks[(size_t)pl].name + " (left) and " + s.tracks[(size_t)pr].name + " (right) into stereo   C", true);
  menuAdd(m, 13, "Rename\xe2\x80\xa6", true);
  if (!stereo && !combined && partner < 0 && !pair)
    menuAdd(m, 2, "Mono. To make it stereo: select it and its other side, then press C", false);
  int cmd = TrackPopupMenu(m, TPM_NONOTIFY | TPM_RETURNCMD, screen.x, screen.y, 0, st->hwnd, NULL);
  DestroyMenu(m);
  if (cmd == 10) {
    pushUndo(st);
    swapTrack(st, idx);
  } else if (cmd == 11 || cmd == 12) {
    pushUndo(st);
    toggleStereo(st, idx);
  } else if (cmd == 14) {
    combineSelected(st);
  } else if (cmd == 13) {
    for (auto &h : st->hits)
      if (h.id == H_TRACK_NAME && h.idx == idx) {
        st->selTracks.clear();
        st->selTracks.insert(idx);
        st->editTrack = idx;
        st->nameRc = h.r;
        startEdit(st, 4, h.r, tr.name, std::string());
        break;
      }
  }
  InvalidateRect(st->hwnd, NULL, FALSE);
}

// The file of an event (the first one; embedded audio: the AAF), for Show in Finder / Explorer and Copy path.
// `found` false: it is missing and the path is the one the AAF gave.
std::string clipFilePath(State *st, const Clip &c, int side, bool *found) {
  const Session &s = st->imp->session();
  *found = false;
  if (side < 0 || side >= (int)c.sources.size()) return std::string();
  int mi = c.sources[(size_t)side].media;
  if (mi < 0 || mi >= (int)s.media.size()) return std::string();
  const Media &md = s.media[(size_t)mi];
  if (md.kind == MediaKind::Embedded) {
    *found = true;
    return s.aafPath;
  }
  if (!md.foundPath.empty()) {
    *found = true;
    return md.foundPath;
  }
  return uriToPath(md.originalUri);
}

const Clip *selectedClip(State *st) {
  if (st->sel.empty()) return nullptr;
  return st->imp->findClip(*st->sel.begin());
}

// Shift + F: the selected event's file in the Finder / Explorer; Shift + C: its path to the clipboard
void revealSelected(State *st) {
  static std::string tip;
  const Clip *c = selectedClip(st);
  bool found = false;
  std::string path = c ? clipFilePath(st, *c, 0, &found) : std::string();
  if (!c) tip = "Select an event first: Shift + F shows its file in the " +
                std::string(
#ifdef _WIN32
                    "Explorer"
#else
                    "Finder"
#endif
                    ) + ", Shift + C copies its path.";
  else if (!found) tip = "Its file is missing, so it can't be shown. Right-click the event to locate it.";
  else {
    if (st->host && st->host->revealFile) st->host->revealFile(path);
    tip = "Shown: " + path;
  }
  st->tip = tip.c_str();
}

void copySelectedPath(State *st) {
  static std::string tip;
  const Clip *c = selectedClip(st);
  bool found = false;
  std::string path = c ? clipFilePath(st, *c, 0, &found) : std::string();
  if (!c || path.empty()) tip = "Select an event first: Shift + C copies the path of its file.";
  else {
    if (st->host && st->host->copyText) st->host->copyText(st->hwnd, path);
    tip = (found ? "Path copied: " : "Path from the AAF copied (the file is missing): ") + path;
  }
  st->tip = tip.c_str();
}

// Right-click on an event: where its file is, show it in the Finder / Explorer, copy the path
void clipMenu(State *st, POINT screen, int clipId) {
  Session &s = st->imp->session();
  Clip *c = st->imp->findClip(clipId);
  if (!c || !st->host) return;
#ifdef _WIN32
  const char *showIn = "Show in Explorer";
#elif defined(__APPLE__)
  const char *showIn = "Show in Finder";
#else
  const char *showIn = "Show in the file manager";
#endif
  std::vector<int> media; // each file once (a stereo pair: two)
  for (auto &src : c->sources)
    if (src.media >= 0 && src.media < (int)s.media.size() &&
        std::find(media.begin(), media.end(), src.media) == media.end())
      media.push_back(src.media);
  if (media.empty()) return;
  HMENU m = CreatePopupMenu();
  int idDis = 1; // grayed lines: information only
  for (size_t k = 0; k < media.size(); k++) {
    const Media &md = s.media[(size_t)media[k]];
    if (k) menuSep(m);
    std::string side = media.size() == 2 ? (k == 0 ? "  (left)" : "  (right)") : "";
    std::string name = md.name.empty() ? baseName(uriToPath(md.originalUri)) : md.name;
    menuAdd(m, idDis++, name + side, false);
    if (md.kind == MediaKind::Embedded) {
      menuAdd(m, idDis++, "Inside the AAF (written out as a file on import)", false);
      menuAdd(m, 100 + (int)k, std::string(showIn).replace(0, 4, "Show the AAF"), true);
    } else if (!md.foundPath.empty()) {
      menuAdd(m, idDis++, shortPath(md.foundPath, 90), false);
      std::string why = st->peaks.why(media[k]);
      if (!why.empty()) menuAdd(m, idDis++, "No waveform: " + why, false);
      menuAdd(m, 100 + (int)k, std::string(showIn) + (k == 0 ? "\tShift+F" : ""), true);
      menuAdd(m, 200 + (int)k, std::string("Copy the file path") + (k == 0 ? "\tShift+C" : ""), true);
    } else {
      menuAdd(m, idDis++, "Missing: " + shortPath(uriToPath(md.originalUri), 80), false);
      menuAdd(m, 300 + (int)k, "Locate\xe2\x80\xa6", true);
      menuAdd(m, 200 + (int)k, "Copy the path from the AAF", true);
    }
  }
  if (clipIsStereo(st, *c)) {
    menuSep(m);
    menuAdd(m, 900, c->swapLR ? "Put left and right back (this event)" : "Swap left and right (this event)", true);
  }
  int cmd = TrackPopupMenu(m, TPM_NONOTIFY | TPM_RETURNCMD, screen.x, screen.y, 0, st->hwnd, NULL);
  DestroyMenu(m);
  if (cmd == 900) {
    pushUndo(st);
    c->swapLR = !c->swapLR;
    st->edited = true;
    if (st->playClip || st->playTL) stopPlay(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return;
  }
  static std::string tipBuf;
  if (cmd >= 100 && cmd < 400) {
    size_t k = (size_t)(cmd % 100);
    if (k >= media.size()) return;
    const Media &md = s.media[(size_t)media[k]];
    std::string path = md.kind == MediaKind::Embedded ? s.aafPath : md.foundPath;
    if (cmd < 200) {
      if (st->host->revealFile) st->host->revealFile(path);
    } else if (cmd < 300) {
      if (path.empty()) path = uriToPath(md.originalUri);
      if (st->host->copyText) st->host->copyText(st->hwnd, path);
      tipBuf = "Path copied: " + path;
      st->tip = tipBuf.c_str();
    } else
      locateOne(st, media[k]);
  }
  InvalidateRect(st->hwnd, NULL, FALSE);
}

// Z: zooms to the selected events (their length fills the lanes) and puts the playhead at their start
void zoomToSelection(State *st) {
  double a = 1e300, b = -1e300;
  int firstTrack = -1;
  for (int id : st->sel) {
    int t = -1;
    if (Clip *c = st->imp->findClip(id, &t)) {
      if (c->pos < a) firstTrack = t;
      a = std::min(a, c->pos);
      b = std::max(b, c->pos + c->len);
    }
  }
  if (b < a) {
    st->tip = "Select an event first: Z zooms to it.";
    return;
  }
  double len = std::max(0.2, b - a), pad = len * 0.04;
  st->viewStart = a - pad;
  st->viewLen = len + 2 * pad;
  bool was = st->playTL;
  if (was || st->playClip) stopPlay(st);
  st->playhead = a;
  if (firstTrack >= 0) { // its track in view too
    buildRows(st);
    int r = rowOfPos(st, posOfTrack(st, firstTrack));
    if (r >= 0 && (r < st->scrollRow || r >= st->scrollRow + kRows)) st->scrollRow = std::max(0, r - kRows / 2);
  }
}

void endEdit(State *st, bool apply);
void startEdit(State *st, int kind, RECT r, const std::string &text, const std::string &key);

// Tab / Shift+Tab: the next / previous track (as shown). While a name is being typed it is kept and
// the next track's name opens, so a whole list can be renamed in one go.
void trackStep(State *st, int dir) {
  bool renaming = st->editKind == 4;
  int from = renaming ? st->editTrack : -1;
  if (renaming) endEdit(st, true);
  buildRows(st);
  int n = (int)st->order.size();
  if (n == 0) return;
  if (from < 0) // the selected track furthest in that direction
    for (int t : st->selTracks) {
      int p = posOfTrack(st, t);
      if (p >= 0 && (from < 0 || (dir > 0 ? p > posOfTrack(st, from) : p < posOfTrack(st, from)))) from = t;
    }
  int pos = from >= 0 ? posOfTrack(st, from) + dir : (dir > 0 ? 0 : n - 1);
  if (pos < 0 || pos >= n) {
    st->tip = dir > 0 ? "That was the last track." : "That was the first track.";
    InvalidateRect(st->hwnd, NULL, FALSE);
    return;
  }
  int t = st->order[(size_t)pos];
  openFolderOf(st, t);
  st->selTracks.clear();
  st->selTracks.insert(t);
  int r = rowOfPos(st, pos); // into view
  if (r >= 0 && (r < st->scrollRow || r >= st->scrollRow + kRows)) st->scrollRow = std::max(0, r - kRows / 2);
  InvalidateRect(st->hwnd, NULL, FALSE);
  if (!renaming) return;
  UpdateWindow(st->hwnd); // painted now: the name field goes where the name is drawn
  for (auto &h : st->hits)
    if (h.id == H_TRACK_NAME && h.idx == t) {
      st->editTrack = t;
      st->nameRc = h.r;
      startEdit(st, 4, h.r, st->imp->session().tracks[(size_t)t].name, std::string());
      return;
    }
}

// Would moving the selection by `delta` tracks put an event on top of one already there?
// (Touching, or an overlap shorter than a frame, is fine.) Says which in `why`.
bool moveBlocked(State *st, int delta, std::string *why) {
  if (!delta || st->sel.empty()) return false;
  Session &s = st->imp->session();
  const double tol = 1.0 / std::max(1.0, s.fps > 0 ? s.fps : 25.0);
  int lo = 1 << 30;
  for (int id : st->sel) {
    int t = -1;
    if (st->imp->findClip(id, &t)) {
      int p = posOfTrack(st, t);
      if (p >= 0) lo = std::min(lo, p);
    }
  }
  if (lo == 1 << 30) return false;
  delta = std::max(delta, -lo);
  for (int id : st->sel) {
    int t = -1;
    Clip *c = st->imp->findClip(id, &t);
    if (!c || c->excluded) continue;
    int to = posOfTrack(st, t) + delta;
    if (to < 0 || to >= (int)st->order.size()) continue; // a new track: free
    const Track &tt = s.tracks[(size_t)st->order[(size_t)to]];
    for (auto &o : tt.clips) {
      if (st->sel.count(o.id) || o.excluded) continue;
      if (o.pos < c->pos + c->len - tol && c->pos < o.pos + o.len - tol) {
        if (why)
          *why = "That spot on " + tt.name + " is taken by " + (o.name.empty() ? std::string("another event") : o.name) +
                 " (" + formatTC(o.pos + tcOffset(st), s.fps) + "). Nothing moved.";
        return true;
      }
    }
  }
  return false;
}

// Where the selection really goes when moved by `delta` tracks: on a taken spot it goes on in the
// same direction to the next track that is free there (below the last track: a new one). 0 = no free
// track (upwards). `note` says what happened.
int freeDelta(State *st, int delta, std::string *note) {
  if (note) note->clear();
  if (!delta || st->sel.empty()) return 0;
  std::string why;
  if (!moveBlocked(st, delta, &why)) return delta;
  int lo = 1 << 30, firstPos = -1;
  double firstT = 1e300;
  for (int id : st->sel) {
    int t = -1;
    if (Clip *c = st->imp->findClip(id, &t)) {
      int p = posOfTrack(st, t);
      if (p < 0) continue;
      lo = std::min(lo, p);
      if (c->pos < firstT) {
        firstT = c->pos;
        firstPos = p;
      }
    }
  }
  int dir = delta > 0 ? 1 : -1, n = (int)st->order.size();
  std::string taken = why.substr(0, why.find(". Nothing moved."));
  for (int d = delta + dir; dir > 0 || lo + d >= 0; d += dir) {
    if (moveBlocked(st, d, nullptr)) continue;
    if (note) {
      int to = firstPos + d;
      std::string where = to >= 0 && to < n ? st->imp->session().tracks[(size_t)st->order[(size_t)to]].name : "";
      *note = taken + ": " + (st->sel.size() == 1 ? "the event" : "the events") +
              (where.empty() ? " went to a new track." : " went to " + where + ", the next free track.");
    }
    return d;
  }
  if (note) *note = taken + ". There's no free track above: nothing moved.";
  return 0;
}

// moves the selection by `delta` tracks in the order the tracks are shown; below the last
// track new tracks are added (outside any folder, so they show up at the bottom)
void moveSelectionBy(State *st, int delta) {
  if (!delta || st->sel.empty()) return;
  static std::string moveNote;
  buildRows(st);
  delta = freeDelta(st, delta, &moveNote); // never on top of another event: on to the next free track
  st->tip = moveNote.empty() ? nullptr : moveNote.c_str();
  if (!delta) return;
  Session &s = st->imp->session();
  buildRows(st);
  int lo = 1 << 30, hi = -1;
  std::vector<std::pair<int, int>> byPos; // (shown position, clip id)
  for (int id : st->sel) {
    int t = -1;
    if (st->imp->findClip(id, &t)) {
      int p = posOfTrack(st, t);
      if (p < 0) continue;
      lo = std::min(lo, p);
      hi = std::max(hi, p);
      byPos.push_back({p, id});
    }
  }
  if (hi < 0) return;
  delta = std::max(delta, -lo);
  if (!delta) return;
  int need = hi + delta - ((int)st->order.size() - 1);
  for (int k = 0; k < need; k++) {
    int nt = st->imp->addTrack(-1);
    s.tracks[(size_t)nt].group.clear();
  }
  buildRows(st);
  for (auto &p : byPos) {
    int to = st->order[(size_t)(p.first + delta)];
    st->imp->moveClips({p.second}, to, st->moveAutomation);
    openFolderOf(st, to); // moved into a closed folder: it opens, so they stay in sight
  }
  st->edited = true;
  buildRows(st);
  // keep the moved events in view
  int rTop = rowOfPos(st, lo + delta), rBot = rowOfPos(st, hi + delta);
  if (rBot >= st->scrollRow + kRows) st->scrollRow = rBot - kRows + 1;
  if (rTop < st->scrollRow) st->scrollRow = rTop;
}

// moves the selected tracks to `slot` (between two rows): into the folder there, in that order
void dropTracks(State *st, int slot) {
  Session &s = st->imp->session();
  buildRows(st);
  std::string key = dropKey(st, slot);
  std::vector<int> moved, rest;
  for (int t : st->order) (st->selTracks.count(t) ? moved : rest).push_back(t);
  if (moved.empty()) return;
  // how many of the remaining tracks are shown above the slot
  size_t ins = 0;
  for (int i = 0; i < slot && i < (int)st->rows.size(); i++)
    if (st->rows[(size_t)i].kind == 0 && !st->selTracks.count(st->rows[(size_t)i].track)) ins++;
  if (slot > (int)st->rows.size() - 1) ins = rest.size();
  for (int t : moved) s.tracks[(size_t)t].group = st->opt->folders ? key : s.tracks[(size_t)t].group;
  std::vector<int> neworder(rest.begin(), rest.begin() + (long)ins);
  neworder.insert(neworder.end(), moved.begin(), moved.end());
  neworder.insert(neworder.end(), rest.begin() + (long)ins, rest.end());
  st->imp->reorderTracks(neworder);
  st->selTracks.clear();
  for (size_t i = 0; i < moved.size(); i++) st->selTracks.insert((int)(ins + i));
  st->folderTrack = st->palTrack = -1;
  buildRows(st);
}

void keepTrackChoices(State *st, const std::function<void()> &rebuild) {
  Session &s = st->imp->session();
  struct Keep { std::string name, group; bool import; uint32_t color; std::string target, targetName; };
  std::vector<Keep> keep;
  for (auto &t : s.tracks) keep.push_back({t.name, t.group, t.import, t.color, t.target, t.targetName});
  rebuild();
  for (auto &t : s.tracks)
    for (auto &k : keep)
      if (k.name == t.name) {
        t.import = k.import;
        t.color = k.color;
        if (k.group.empty() || s.folder(k.group)) t.group = k.group;
        t.target = k.target;
        t.targetName = k.targetName;
      }
}

const int IDC_TCEDIT = 1001;

void startEdit(State *st, int kind, RECT r, const std::string &text, const std::string &key = std::string()) {
  if (!st->tcEdit) {
#ifdef _WIN32
    st->tcEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, st->hwnd,
                                 (HMENU)(INT_PTR)IDC_TCEDIT, NULL, NULL);
#else
    SWELL_MakeSetCurParms(1.0f, 1.0f, 0, 0, st->hwnd, false, false);
    st->tcEdit = SWELL_MakeEditField(IDC_TCEDIT, 0, 0, 10, 10, ES_AUTOHSCROLL);
    SWELL_MakeSetCurParms(1.0f, 1.0f, 0, 0, NULL, false, false);
#endif
    if (!st->tcEdit) return;
    SendMessage(st->tcEdit, WM_SETFONT, (WPARAM)st->fBody, 0);
  }
  st->editRc = r;
  st->editKind = kind;
  st->editKey = key;
  SetWindowPos(st->tcEdit, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
  olSetText(st->tcEdit, text.c_str());
  ShowWindow(st->tcEdit, SW_SHOW);
  SetFocus(st->tcEdit);
  SendMessage(st->tcEdit, EM_SETSEL, 0, -1);
  InvalidateRect(st->hwnd, NULL, FALSE);
}

void endEdit(State *st, bool apply) {
  if (!st->editKind || !st->tcEdit) return;
  int kind = st->editKind;
  st->editKind = 0; // first, so the focus change below does not come back here
  std::string v = olGetText(st->tcEdit);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
  while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
  if (apply && kind == 1) {
    double sec;
    if (parseTC(v, st->imp->session().fps, &sec) && sec >= 0) { // where the AAF starts in REAPER
      double d = sec - st->imp->session().firstClip();
      st->tcAbs = std::fabs(d);
      st->tcMinus = d < 0;
      st->tip = nullptr;
    } else
      st->tip = "That is not a timecode. Use HH:MM:SS:FF or just the digits.";
  }
  if (apply && (kind == 2 || kind == 4) && !v.empty()) pushUndo(st);
  if (apply && kind == 2)
    if (Folder *f = st->imp->session().folder(st->editKey))
      if (!v.empty()) f->name = v;
  if (apply && kind == 3 && !v.empty()) st->opt->subdir = safeFileName(v);
  if (apply && (kind == 5 || kind == 6)) { // a sorting rule's words / name, typed in the panel
    std::vector<SortRule> rules = sortRules();
    size_t i = (size_t)atoi(st->editKey.c_str());
    if (i < rules.size()) {
      if (kind == 5) {
        for (auto &ch : v)
          if (ch == '|') ch = ',';
        rules[i].words = splitWords(v);
        saveRules(st, rules);
      } else if (!v.empty()) {
        bool taken = v.find_first_of("|;") != std::string::npos;
        for (size_t k = 0; k < rules.size(); k++)
          if (k != i && lower(rules[k].name) == lower(v)) taken = true;
        if (taken) st->tip = "That folder name is taken (or has | or ;).";
        else {
          rules[i].name = v;
          saveRules(st, rules);
        }
      }
    }
  }
  if (apply && kind == 4 && !v.empty() && st->editTrack >= 0 && st->editTrack < (int)st->imp->session().tracks.size())
    st->imp->session().tracks[(size_t)st->editTrack].name = v;
  ShowWindow(st->tcEdit, SW_HIDE);
  SetFocus(st->hwnd);
  InvalidateRect(st->hwnd, NULL, FALSE);
}

// the options every reel shares (not where it lands, its .L / .R setting or its picture)
void copyShared(const ImportOptions &from, ImportOptions &to) {
  ImportOptions keep = to;
  to = from;
  to.tcOffset = keep.tcOffset;
  to.mergeStereo = keep.mergeStereo;
  to.video = keep.video;
  to.read = keep.read;
}

void restoreChoices(State *st);
void stopPlay(State *st);

// a reel shown for the first time: its whole edit in view, its last choices back
void initReelView(State *st) {
  restoreChoices(st);
  double a, b;
  sessionRange(st->imp->session(), &a, &b);
  st->viewStart = a - (b - a) * 0.02;
  st->viewLen = (b - a) * 1.04;
  st->tcAbs = std::fabs(st->opt->tcOffset);
  st->tcMinus = st->opt->tcOffset < 0;
  st->scrollRow = 0;
  st->playhead = -1;
  st->sel.clear();
  st->solo.clear();
  st->undo.clear();
  st->redo.clear();
  st->edited = false;
  st->freshNow = false;
}

void switchReel(State *st, int to) {
  int n = reelCount(st);
  if (to < 0 || to >= n || to == st->reel || reelHidden(st, to)) return;
  Importer *imp;
  ImportOptions *opt;
  if (!st->host || !st->host->reelAt || !st->host->reelAt(to, &imp, &opt)) return;
  stopPlay(st);
  if (st->editKind) endEdit(st, true);
  st->optsOpen = st->helpOpen = st->missingOpen = st->rulesOpen = st->themeMenu = false;
  st->palTrack = st->folderTrack = st->destTrack = -1;
  st->palFolder.clear();
  st->opt->tcOffset = tcOffset(st);
  if ((int)st->reelViews.size() < n) st->reelViews.resize((size_t)n);
  State::ReelView &v = st->reelViews[(size_t)st->reel];
  v.inited = true;
  v.scrollRow = st->scrollRow;
  v.viewStart = st->viewStart;
  v.viewLen = st->viewLen;
  v.tcAbs = st->tcAbs;
  v.tcMinus = st->tcMinus;
  v.playhead = st->playhead;
  v.edited = st->edited;
  v.merge = st->merge;
  v.choicesBack = st->choicesBack;
  v.freshNow = st->freshNow;
  v.sel = st->sel;
  v.solo = st->solo;
  v.undo = std::move(st->undo);
  v.redo = std::move(st->redo);
  v.fresh = st->fresh;
  copyShared(*st->opt, *opt);
  st->reel = to;
  st->imp = imp;
  st->opt = opt;
  State::ReelView &w = st->reelViews[(size_t)to];
  if (!w.inited) {
    ImportOptions shared = *opt;
    initReelView(st); // its own last choices may set other options: the shared ones stay as shown
    copyShared(shared, *opt);
  } else {
    st->scrollRow = w.scrollRow;
    st->viewStart = w.viewStart;
    st->viewLen = w.viewLen;
    st->tcAbs = w.tcAbs;
    st->tcMinus = w.tcMinus;
    st->playhead = w.playhead;
    st->edited = w.edited;
    st->merge = w.merge;
    st->choicesBack = w.choicesBack;
    st->freshNow = w.freshNow;
    st->sel = w.sel;
    st->solo = w.solo;
    st->undo = std::move(w.undo);
    st->redo = std::move(w.redo);
    st->fresh = w.fresh;
  }
  st->merge = st->opt->mergeStereo;
  buildRows(st);
  st->peaks.start(st->imp->session());
  st->peakSeen = -1;
  if (st->hwnd) SetTimer(st->hwnd, kPeakTimer, 150, NULL);
  embedCheck(st);
}

// the last import's choices for this AAF, when it is the same edit
void restoreChoices(State *st) {
  Session &s = st->imp->session();
  st->fresh = {s.tracks, s.folders, s.video, *st->opt};
  if (!st->host || !st->host->loadChoices) return;
  std::string text = st->host->loadChoices(choicesFileName(s.aafPath));
  if (text.empty()) return;
  std::string why;
  st->choicesBack = applyChoices(text, *st->imp, *st->opt, &why);
  st->merge = st->opt->mergeStereo;
}

// closes the window: modal -> EndDialog, modeless -> destroy it, then hand the result on
void closeDialog(State *st, bool accepted) {
  st->accepted = accepted;
  st->opt->tcOffset = tcOffset(st);
  for (int i = 0; i < reelCount(st); i++) { // every reel: the shared options, and its choices kept for next time
    Importer *imp;
    ImportOptions *opt;
    if (!reelAt(st, i, &imp, &opt) || reelHidden(st, i)) continue;
    if (i != st->reel) copyShared(*st->opt, *opt);
    if (accepted && st->host && st->host->saveChoices)
      st->host->saveChoices(choicesFileName(imp->session().aafPath), choicesToText(imp->session(), *opt));
  }
  if (!st->modeless) {
    EndDialog(st->hwnd, accepted ? 1 : 0);
    return;
  }
  std::function<void(bool)> cb = st->onClose;
  DestroyWindow(st->hwnd); // deletes st (WM_DESTROY)
  if (cb) cb(accepted);
}

// horizontal zoom by `factor` (< 1 zooms in), keeping the time under layout x `ax` in place
void zoomBy(State *st, double factor, double ax) {
  double tlw = kTlr - kTlx;
  double rel = std::max(0.0, std::min(1.0, (ax - kTlx) / tlw));
  double t = st->viewStart + rel * st->viewLen;
  double total = std::max(1.0, st->rangeB - st->rangeA);
  double len = std::max(0.5, std::min(st->viewLen * factor, total * 1.5));
  st->viewStart = t - rel * len;
  st->viewLen = len;
}

bool shiftDown() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }
bool altDown() { return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0; } // Option on macOS
bool ctrlDown() { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; } // Cmd on macOS

void stopPlay(State *st);
void togglePlay(State *st);
void toggleTimeline(State *st);

void click(State *st, int x, int y, bool shift) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  const Hit *h = hitAt(st, x, y);
  int id = h ? h->id : H_NONE;
  if (st->reelAsk && id != H_REEL_SHARE && id != H_REEL_OWN) return; // answer the question first
  if (!st->reelAsk && st->embedAsk) {
    if (id == H_EMBED_LOAD || id == H_EMBED_SKIP) {
      embedAnswer(st, id == H_EMBED_LOAD);
      InvalidateRect(st->hwnd, NULL, FALSE);
    }
    return;
  }
  bool inFolderPop = id == H_FOLD_ASSIGN || id == H_FOLD_NONE || id == H_FOLD_RENAME || id == H_FOLD_DELETE ||
                     id == H_FOLD_NEW || id == H_FOLD_POP || id == H_FOLD_COLOR;
  bool inPal = id == H_PAL || id == H_PAL_CUSTOM || id == H_PAL_NONE || id == H_PAL_BG;
  bool palOpen = st->palTrack >= 0 || !st->palFolder.empty();
  // a click outside a popover closes it; a click on empty space only closes it
  if (palOpen && !inPal && id != H_TRACK_COLOR && id != H_FOLD_COLOR && id != H_FROW_COLOR) {
    st->palTrack = -1;
    st->palFolder.clear();
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE || id == H_FOLD_POP) return;
  }
  if (isEditHit(id)) pushUndo(st);
  bool inOpts = id == H_OPTS_BG || id == H_FADES || id == H_MERGE || id == H_MARKERS || id == H_FOLDERS || id == H_SAMPLES || id == H_GAIN_ITEMS ||
                id == H_GAIN_TRACK || id == H_GAIN_OFF || id == H_VIDEO || id == H_VIDEO_CHOOSE || id == H_COPY_YES ||
                id == H_COPY_NO || id == H_SUBDIR || id == H_DEST_CHOOSE || id == H_LOCATE;
  if (st->helpOpen && id != H_HELP_BG && id != H_HELP) { // a click anywhere else closes the help
    st->helpOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    return;
  }
  if (st->themeMenu && id != H_THEME && id != H_THEME_BG && id != H_THEME_PICK) {
    st->themeMenu = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  if (st->rulesOpen && !(id >= H_RULES && id <= H_RULES_CLOSE)) {
    st->rulesOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  if (st->missingOpen && id != H_MISS_BG && id != H_MISS_PICK && id != H_MISS_OPEN && id != H_LOCATE && id != H_MISS_PAGE && id != H_MISS_CLOSE) {
    st->missingOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  if (st->optsOpen && !inOpts && id != H_OPTS) {
    st->optsOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  bool inDest = id == H_DEST_NEW || id == H_DEST_MATCHALL || id == H_DEST_PICK || id == H_DEST_POP || id == H_DEST_OVERWRITE;
  if (st->destTrack >= 0 && !inDest && id != H_TRACK_DEST) {
    st->destTrack = -1;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  if (st->folderTrack >= 0 && !inFolderPop && !inPal && id != H_TRACK_FOLDERMENU) {
    st->folderTrack = -1;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LANES || id == H_NONE) return;
  }
  switch (id) {
  case H_LANES: {
    const ClipHit *c = clipAt(st, x, y);
    if (st->playClip && (!c || c->id != st->playClip)) stopPlay(st);
    if (!st->playTL && !shift && c == nullptr) { // a click on empty lane space puts the playhead there
      double spp = st->viewLen / (kTlr - kTlx);
      st->playhead = std::max(0.0, st->viewStart + ((x - st->ox) / st->scale - kTlx) * spp);
    }
    if (!c) {
      if (!shift) st->sel.clear();
    } else if (shift) {
      if (st->sel.count(c->id)) st->sel.erase(c->id);
      else st->sel.insert(c->id);
    } else {
      st->sel.clear();
      st->sel.insert(c->id);
    }
    break;
  }
  case H_TRACK_CHECK: s.tracks[(size_t)h->idx].import = !s.tracks[(size_t)h->idx].import; break;
  case H_TRACK_NAME: {
    int t = h->idx;
    if (shift || ctrlDown()) {
      if (st->selTracks.count(t)) st->selTracks.erase(t);
      else st->selTracks.insert(t);
    } else {
      st->selTracks.clear();
      st->selTracks.insert(t);
    }
    break;
  }
  case H_TRACK_DEST: {
    bool same = st->destTrack == h->idx;
    st->destTrack = same ? -1 : h->idx;
    st->folderTrack = st->palTrack = -1;
    st->palFolder.clear();
    if (!same) {
      st->projTracks = st->host->projectTracks ? st->host->projectTracks() : decltype(st->projTracks)();
      st->destScroll = 0;
    }
    st->dpX = (h->r.left - st->ox) / st->scale - 120;
    st->dpY = (h->r.bottom - st->oy) / st->scale + 2;
    st->dpY0 = (h->r.top - st->oy) / st->scale - 2;
    break;
  }
  case H_DEST_NEW:
  case H_DEST_PICK: {
    // the clicked track, or every selected track when it is one of them
    std::set<int> which;
    if (st->selTracks.count(st->destTrack)) which = st->selTracks;
    else which.insert(st->destTrack);
    for (int t : which) {
      Track &tr = s.tracks[(size_t)t];
      if (id == H_DEST_NEW) {
        tr.target.clear();
        tr.targetName.clear();
      } else {
        tr.target = st->projTracks[(size_t)h->idx].first;
        tr.targetName = st->projTracks[(size_t)h->idx].second;
        tr.group.clear(); // an existing track is not put into a new folder
      }
    }
    st->destTrack = -1;
    break;
  }
  case H_DEST_MATCHALL: {
    int n = 0;
    for (auto &tr : s.tracks) {
      for (auto &p : st->projTracks)
        if (lower(p.second) == lower(tr.name)) {
          tr.target = p.first;
          tr.targetName = p.second;
          tr.group.clear();
          n++;
          break;
        }
    }
    static std::string tipBuf;
    tipBuf = n ? strprintf("%d track%s matched by name.", n, n == 1 ? "" : "s") : std::string("No track names match your project.");
    st->tip = tipBuf.c_str();
    st->destTrack = -1;
    break;
  }
  case H_DEST_POP: return;
  case H_DEST_OVERWRITE: opt.overwriteEnvelopes = !opt.overwriteEnvelopes; break;
  case H_TRACK_FOLDERMENU: {
    bool same = st->folderTrack == h->idx;
    st->folderTrack = same ? -1 : h->idx;
    st->palTrack = -1;
    st->palFolder.clear();
    st->fpX = std::min((h->r.left - st->ox) / st->scale, (double)kL + 20);
    st->fpY = (h->r.bottom - st->oy) / st->scale + 2;
    st->fpY0 = (h->r.top - st->oy) / st->scale - 2;
    break;
  }
  case H_TRACK_DELETE:
    if (st->imp->removeCreatedTrack(h->idx, st->moveAutomation)) {
      st->selTracks.clear();
      st->folderTrack = st->palTrack = -1;
      st->palFolder.clear();
    }
    break;
  case H_NEW_FOLDER: {
    // a new folder, with the selected tracks in it; then type its name
    std::string key = st->imp->addFolder("New folder");
    for (int t : st->selTracks) s.tracks[(size_t)t].group = key;
    opt.folders = true;
    st->selTracks.clear();
    buildRows(st);
    int ri = 0;
    for (size_t i = 0; i < st->rows.size(); i++)
      if (st->rows[i].kind == 1 && st->rows[i].folder == key) ri = (int)i;
    if (ri < st->scrollRow || ri >= st->scrollRow + kRows) st->scrollRow = std::max(0, ri - 2);
    double ry = kLT + (ri - st->scrollRow) * kRowH;
    startEdit(st, 2, devRect(st, kL + 26, ry + 4, kL + kHdr - 32, ry + kRowH - 4), "New folder", key);
    return;
  }
  case H_TRACK_COLOR: {
    bool same = st->palTrack == h->idx;
    st->palTrack = same ? -1 : h->idx;
    st->palFolder.clear();
    st->folderTrack = -1;
    st->palX = (h->r.right - st->ox) / st->scale + 2;
    st->palY = (h->r.bottom - st->oy) / st->scale;
    st->palY0 = (h->r.top - st->oy) / st->scale;
    break;
  }
  case H_FOLD_COLOR:
  case H_FROW_COLOR: {
    std::string key = s.folders[(size_t)h->idx].key;
    bool same = st->palFolder == key;
    st->palFolder = same ? std::string() : key;
    st->palTrack = -1;
    st->palX = (h->r.right - st->ox) / st->scale + 2;
    st->palY = (h->r.bottom - st->oy) / st->scale;
    st->palY0 = (h->r.top - st->oy) / st->scale;
    break;
  }
  case H_PAL:
  case H_PAL_NONE:
  case H_PAL_CUSTOM: {
    uint32_t *target = nullptr;
    if (!st->palFolder.empty()) {
      if (Folder *f = s.folder(st->palFolder)) target = &f->color;
    } else if (st->palTrack >= 0 && st->palTrack < (int)s.tracks.size())
      target = &s.tracks[(size_t)st->palTrack].color;
    if (target) {
      if (id == H_PAL) *target = kPalette[h->idx];
      else if (id == H_PAL_NONE) *target = 0;
      else {
        uint32_t c = *target;
        if (st->host->pickColor && st->host->pickColor(st->hwnd, &c)) *target = c ? c : 1;
      }
    }
    st->palTrack = -1; // the folder popover stays open after picking a folder color
    st->palFolder.clear();
    break;
  }
  case H_PAL_BG: return;
  case H_FOLD_ASSIGN:
    s.tracks[(size_t)st->folderTrack].group = s.folders[(size_t)h->idx].key;
    st->folderTrack = -1;
    break;
  case H_FOLD_NONE:
    s.tracks[(size_t)st->folderTrack].group.clear();
    st->folderTrack = -1;
    break;
  case H_FOLD_DELETE: st->imp->removeFolder(s.folders[(size_t)h->idx].key); break;
  case H_FOLD_RENAME: {
    const Folder &f = s.folders[(size_t)h->idx];
    startEdit(st, 2, folderNameRect(st, h->idx), f.name, f.key);
    return;
  }
  case H_FROW_RENAME: {
    const Folder &f = s.folders[(size_t)h->idx];
    RECT r = h->r;
    RECT nr = devRect(st, kL + 26, (r.top - st->oy) / st->scale - 0, kL + kHdr - 32, (r.bottom - st->oy) / st->scale);
    startEdit(st, 2, nr, f.name, f.key);
    return;
  }
  case H_FOLD_NEW: {
    std::string key = st->imp->addFolder("New folder");
    s.tracks[(size_t)st->folderTrack].group = key;
    startEdit(st, 2, folderNameRect(st, (int)s.folders.size() - 1), "New folder", key);
    return;
  }
  case H_FOLD_POP: return;
  case H_ZOOM_FIT: {
    double a, b;
    sessionRange(s, &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
    break;
  }
  case H_ZOOM_IN: zoomBy(st, 0.5, (kTlx + kTlr) / 2); break;
  case H_ZOOM_OUT: zoomBy(st, 2.0, (kTlx + kTlr) / 2); break;
  case H_MERGE:
    st->undo.clear();
    st->redo.clear(); {
    st->merge = !st->merge;
    keepTrackChoices(st, [&]() { st->imp->setMergeStereo(st->merge); });
    opt.mergeStereo = st->merge;
    st->sel.clear();
    st->edited = false;
    break;
  }
  case H_FADES: opt.fades = !opt.fades; break;
  case H_GAIN: // items -> track envelope -> off -> items
    if (opt.clipGain && !opt.clipGainToTrack) opt.clipGainToTrack = true;
    else if (opt.clipGain) {
      opt.clipGain = false;
      opt.clipGainToTrack = false;
    } else
      opt.clipGain = true;
    break;
  case H_SHOW_AUTO: opt.showAutomation = !opt.showAutomation; break;
  case H_OPTS: st->optsOpen = !st->optsOpen; st->folderTrack = st->destTrack = st->palTrack = -1; st->palFolder.clear(); break;
  case H_OPTS_BG: return;
  case H_THEME: st->themeMenu = !st->themeMenu; break;
  case H_THEME_BG: return;
  case H_THEME_PICK: {
    int m = h ? h->idx : 0;
    olaaf::setThemeMode(m);
    static const char *keys[3] = {"system", "dark", "light"};
    if (st->host && st->host->setSetting) st->host->setSetting("theme", keys[m < 0 || m > 2 ? 0 : m]);
    st->themeMenu = false;
    break;
  }
  case H_MISS_BG: return;
  case H_MISS_CLOSE: st->missingOpen = false; break;
  case H_MISS_PAGE: st->missScroll += (h ? h->idx : 1) * ((int)kMissRows - 1); break;
  case H_REEL: switchReel(st, h ? h->idx : 0); break;
  case H_REEL_SHARE:
  case H_REEL_OWN:
  case H_REEL_MODE: {
    reelAnswer(st, id == H_REEL_SHARE ? true : id == H_REEL_OWN ? false : !opt.reelsShareTracks);
    break;
  }
  case H_REEL_ADD: {
    if (!st->host || !st->host->addReels) break;
    stopPlay(st);
    int before = reelCount(st);
    int added = st->host->addReels(st->hwnd, &st->reelMsg);
    st->reelViews.resize((size_t)reelCount(st));
    if (added > 0) {
      switchReel(st, before);
      if (st->reelMsg.empty())
        st->reelMsg = strprintf("%d reel%s added at %s own timecode.", added, added == 1 ? "" : "s", added == 1 ? "its" : "their");
      askAboutReels(st);
    }
    st->tip = st->reelMsg.empty() ? nullptr : st->reelMsg.c_str();
    break;
  }
  case H_REEL_REMOVE: { // close a tab: that AAF is not imported
    int i = h ? h->idx : 0;
    std::vector<int> sh = shownReels(st);
    if (i < 0 || i >= reelCount(st) || sh.size() < 2 || !st->host || !st->host->removeReel) break;
    static std::string gone;
    {
      Importer *ri;
      ImportOptions *ro;
      gone = reelAt(st, i, &ri, &ro) ? baseName(ri->session().aafPath) : std::string("The AAF");
    }
    if (st->reel == i) { // show a neighbour first
      size_t k = std::find(sh.begin(), sh.end(), i) - sh.begin();
      switchReel(st, k + 1 < sh.size() ? sh[k + 1] : sh[k - 1]);
    }
    int shownNow = st->reel;
    { // its audio may still be unpacking: stop that before the AAF goes away
      Importer *ri;
      ImportOptions *ro;
      if (reelAt(st, i, &ri, &ro)) embedCancel(st, ri);
    }
    st->host->removeReel(i);
    st->laneShow.clear(); // the tabs after it move up
    if (i >= 1) { // gone from the list: the ones after it move up
      if (i < (int)st->reelViews.size()) st->reelViews.erase(st->reelViews.begin() + i);
      if (shownNow > i) shownNow--;
    }
    st->reel = shownNow;
    gone += " is closed and won't be imported.";
    st->tip = gone.c_str();
    break;
  }
  case H_FRESH: { // back to the AAF as it is (undoable, unless the .L / .R setting changes with it)
    if (opt.mergeStereo != st->fresh.opt.mergeStereo) {
      st->undo.clear();
      st->redo.clear();
      st->imp->setMergeStereo(st->fresh.opt.mergeStereo);
    } else
      pushUndo(st);
    s.tracks = st->fresh.tracks;
    s.folders = st->fresh.folders;
    s.video = st->fresh.video;
    {
      bool share = opt.reelsShareTracks; // one setting for all tabs: not part of this tab's fresh start
      opt = st->fresh.opt;
      opt.reelsShareTracks = share;
    }
    st->merge = opt.mergeStereo;
    st->choicesBack = false;
    st->edited = false;
    st->freshNow = true;
    st->sel.clear();
    buildRows(st);
    st->tip = reelCount(st) > 1 ? "Started fresh: this tab's AAF as it is (the other tabs keep their changes)."
                                : "Started fresh: the AAF as it is.";
    break;
  }
  case H_RULES: st->rulesOpen = true; st->optsOpen = false; break;
  case H_RULES_BG: return;
  case H_RULES_CLOSE: st->rulesOpen = false; break;
  case H_RULE_COLOR: { // the system's color picker
    std::vector<SortRule> rules = sortRules();
    int i = h ? h->idx : -1;
    if (i < 0 || i >= (int)rules.size() || !st->host || !st->host->pickColor) break;
    uint32_t c = rules[(size_t)i].color;
    if (!st->host->pickColor(st->hwnd, &c)) break;
    rules[(size_t)i].color = c ? c : 1;
    saveRules(st, rules);
    for (auto &f : st->imp->session().folders) // the folder in this AAF takes the color right away
      if (f.key == rules[(size_t)i].name) f.color = rules[(size_t)i].color;
    break;
  }
  case H_RULE_NAME: case H_RULE_WORDS: { // typed right in the field
    std::vector<SortRule> rules = sortRules();
    int i = h ? h->idx : -1;
    if (i < 0 || i >= (int)rules.size() || !h) break;
    std::string v = rules[(size_t)i].name;
    if (id == H_RULE_WORDS) {
      v.clear();
      for (size_t k = 0; k < rules[(size_t)i].words.size(); k++) v += (k ? ", " : "") + rules[(size_t)i].words[k];
    }
    startEdit(st, id == H_RULE_WORDS ? 5 : 6, h->r, v, std::to_string(i));
    return;
  }
  case H_RULES_RESET: saveRules(st, defaultSortRules()); st->tip = "OLAAF's own folders and words are back."; break;
  case H_RULES_SORT:
    st->imp->assignDefaultFolders(st->opt->subFolders);
    if (!st->opt->folders) st->opt->folders = true;
    st->edited = true;
    buildRows(st);
    st->tip = st->imp->session().folders.empty() ? "The names point to one folder only: the tracks stay as they are."
                                                  : "The tracks are sorted into the folders again.";
    break;
  case H_TPL_CHOOSE: {
    std::string p;
    if (!st->host || !st->host->pickFile || !st->host->pickFile("OLAAF: choose a track template (.RTrackTemplate)", &p) ||
        p.empty())
      break;
    std::vector<olaaf::TemplateTrack> tt;
    std::string err;
    if (!olaaf::readTrackTemplate(p, &tt, &err)) {
      static std::string errTip;
      errTip = err;
      st->tip = errTip.c_str();
      break;
    }
    st->opt->trackTemplate = p;
    if (st->host->setSetting) st->host->setSetting("template", p);
    break;
  }
  case H_TPL_CLEAR:
    st->opt->trackTemplate.clear();
    if (st->host && st->host->setSetting) st->host->setSetting("template", "");
    break;
  case H_MISS_OPEN: st->missingOpen = !st->missingOpen; st->optsOpen = false; break;
  case H_MISS_PICK: locateOne(st, h ? h->idx : -1); break;
  case H_HELP: st->helpOpen = !st->helpOpen; st->optsOpen = false; break;
  case H_HELP_BG: case H_GONIO_BG: return;
  case H_GONIO_CLOSE: st->gonioOpen = false; break;
  case H_GONIO_TOGGLE: st->gonioOpen = !st->gonioOpen; break;
  case H_TRACK_SOLO: {
    int idx = h ? h->idx : -1;
    if (idx < 0 || idx >= (int)s.tracks.size()) break;
    int uid = s.tracks[(size_t)idx].uid;
    bool was = st->solo.count(uid) > 0; // one track at a time
    st->solo.clear();
    if (!was) st->solo.insert(uid);
    if (st->playTL) { // hear it right away
      double at = st->playhead;
      stopPlay(st);
      st->playhead = at;
      toggleTimeline(st);
    }
    break;
  }
  case H_EV_SWAP: {
    bool any = false, all = true;
    for (int sid : st->sel)
      if (Clip *k = st->imp->findClip(sid))
        if (clipIsStereo(st, *k)) {
          any = true;
          all = all && k->swapLR;
        }
    if (!any) break;
    for (int sid : st->sel)
      if (Clip *k = st->imp->findClip(sid))
        if (clipIsStereo(st, *k)) k->swapLR = !all;
    st->edited = true;
    if (st->playClip) stopPlay(st);
    break;
  }
  case H_TRACK_SWAP: swapTrack(st, h ? h->idx : -1); break;
  case H_TRACK_STEREO: toggleStereo(st, h ? h->idx : -1); break;
  case H_HELP_CLOSE: st->helpOpen = false; break;
  case H_HELP_MANUAL:
    if (st->host && st->host->openUrl) st->host->openUrl("https://dmajorsound.com/software/olaaf/manual.html");
    return;
  case H_UPDATE:
    if (st->host && st->host->openUrl && !st->updUrl.empty()) st->host->openUrl(st->updUrl);
    return;
  case H_SAMPLES:
    st->showSamples = !st->showSamples;
    if (st->host && st->host->setSetting) st->host->setSetting("samples", st->showSamples ? "1" : "0");
    break;
  case H_GAIN_ITEMS: opt.clipGain = true; opt.clipGainToTrack = false; break;
  case H_GAIN_TRACK: opt.clipGain = true; opt.clipGainToTrack = true; break;
  case H_GAIN_OFF: opt.clipGain = false; break;
  case H_FPS_OK: st->fpsDismissed = true; break;
  case H_FPS_SHOW: st->fpsDismissed = false; break;
  case H_VIDEO: opt.video = !opt.video; break;
  case H_VIDEO_CHOOSE: { // the AAF has no picture (or a chosen one is replaced): pick the file by hand
    std::string path;
    if (st->host->pickFile && st->host->pickFile("OLAAF: choose the video for this AAF", &path) && !path.empty()) {
      if (!s.video.empty() && !s.video[0].manual) { // the AAF's own (missing) picture: keep its place
        s.video[0].foundPath = path;
        opt.video = true;
        static std::string tipLinked;
        tipLinked = "Video found: " + baseName(path) + ", placed where the AAF puts it.";
        st->tip = tipLinked.c_str();
        break;
      }
      VideoClip v;
      v.foundPath = path;
      v.name = baseName(path);
      // the file's own timecode (MOV / MP4 timecode track, MXF) when it has one, otherwise where the AAF starts
      double tc = 0;
      bool hasTc = videoStartTimecode(path, &tc) && (tc > 0 || s.firstClip() < 1.0);
      v.pos = hasTc ? tc : s.firstClip();
      static std::string tipBuf;
      tipBuf = hasTc ? "Video placed at its own timecode " + formatTC(tc, s.fps) + "."
                     : "The video has no timecode: placed where the AAF starts, " + formatTC(v.pos, s.fps) + ".";
      st->tip = tipBuf.c_str();
      v.manual = true;
      s.video.clear();
      s.video.push_back(v);
      opt.video = true;
    }
    break;
  }
  case H_MARKERS: opt.markers = !opt.markers; break;
  case H_FOLDERS: opt.folders = !opt.folders; st->folderTrack = -1; break;
  case H_COPY_YES: opt.copyToSubdir = true; break;
  case H_COPY_NO: opt.copyToSubdir = false; break;
  case H_SUBDIR: startEdit(st, 3, st->subRc, opt.subdir.empty() ? defaultSubdir(s) : opt.subdir); return;
  case H_DEST_CHOOSE: {
    std::string p;
    if (pickFolder(st->hwnd, "OLAAF: choose where the audio subfolder goes", opt.destDir, &p)) opt.destDir = p;
    break;
  }
  case H_LOCATE: {
    std::string p;
    if (pickFolder(st->hwnd, "OLAAF: choose the folder that holds the media", dirName(s.aafPath), &p)) {
      opt.searchFolders.insert(opt.searchFolders.begin(), p); // searched first, remembered for next time
      st->imp->locateMedia(opt);
      st->mediaRev++;
  st->mediaRev++;
      st->peaks.start(s);
      SetTimer(st->hwnd, kPeakTimer, 150, NULL);
    }
    break;
  }
  case H_REMOVE_EMPTY: { // checked again, track by track, right before deleting
    static std::string done;
    std::vector<std::string> names;
    int n = st->imp->removeEmptyTracks(&names);
    if (n > 0) {
      st->selTracks.clear();
      st->solo.clear();
      st->folderTrack = st->palTrack = st->destTrack = -1;
      st->edited = true;
      std::string list;
      for (size_t i = 0; i < names.size() && i < 6; i++) list += (i ? ", " : "") + names[i];
      if (names.size() > 6) list += strprintf(" and %zu more", names.size() - 6);
      done = strprintf("%d empty track%s deleted: %s. Cmd/Ctrl+Z brings %s back.", n, n == 1 ? "" : "s", list.c_str(),
                       n == 1 ? "it" : "them");
    } else
      done = "No empty tracks: nothing was deleted.";
    st->tip = done.c_str();
    buildRows(st);
    break;
  }
  case H_FOLD_TOGGLE: { // Alt / Option-click: every folder the same way
    int fi = h ? h->idx : -1;
    if (fi < 0 || fi >= (int)s.folders.size()) break;
    bool close = !st->closedFolders.count(s.folders[(size_t)fi].key);
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
      st->closedFolders.clear();
      if (close)
        for (auto &f : s.folders) st->closedFolders.insert(f.key);
    } else if (close)
      st->closedFolders.insert(s.folders[(size_t)fi].key);
    else
      st->closedFolders.erase(s.folders[(size_t)fi].key);
    buildRows(st);
    st->scrollRow = std::max(0, std::min(st->scrollRow, (int)st->rows.size() - kRows));
    break;
  }
  case H_SORT_NOW: // the same as "Sort the tracks now" in the sorting panel
    st->imp->assignDefaultFolders(st->opt->subFolders);
    st->opt->folders = true;
    st->closedFolders.clear();
    st->edited = true;
    buildRows(st);
    st->tip = st->imp->session().folders.empty() ? "The names point to one folder only: the tracks stay as they are."
                                                  : "The tracks are sorted into DX, FX, MX and BG by their names (Cmd/Ctrl+Z undoes it).";
    break;
  case H_SORT_SUBS: { // groups on / off; already sorted: sort again right away
    st->opt->subFolders = !st->opt->subFolders;
    Session &ss = st->imp->session();
    if (st->opt->folders && !ss.folders.empty()) {
      st->imp->assignDefaultFolders(st->opt->subFolders);
      st->closedFolders.clear();
      st->edited = true;
      buildRows(st);
      int subs = 0;
      for (auto &f : ss.folders) subs += !f.parent.empty();
      static std::string msg; // the tip points here
      msg = !st->opt->subFolders ? "Sub groups off: sorted into DX, FX, MX and BG only (Cmd/Ctrl+Z undoes it)."
            : subs ? strprintf("Sorted again: %d group%s got a folder of their own (Cmd/Ctrl+Z undoes it).", subs, subs == 1 ? "" : "s")
                   : std::string("No numbered groups found (like FX A 1, FX A 2): the folders stay as they are.");
      st->tip = msg.c_str();
    } else
      st->tip = st->opt->subFolders ? "Sub groups on: Sort also makes folders for numbered groups like FX A 1, FX A 2."
                                    : "Sub groups off: Sort makes DX, FX, MX and BG only.";
    break;
  }
  case H_SORT_RESET: // no folders, the AAF's track order
    st->imp->clearFolders();
    st->closedFolders.clear();
    st->selTracks.clear();
    st->folderTrack = st->destTrack = st->palTrack = -1;
    st->edited = true;
    buildRows(st);
    st->tip = "Unsorted: no folders, the tracks in the AAF's order (Cmd/Ctrl+Z brings the folders back).";
    break;
  case H_TC_RESET: st->tcAbs = 0; st->tcMinus = false; st->tip = "Back at the AAF's own timecode."; break;
  case H_TC_PLUS: st->tcMinus = false; break;
  case H_TC_MINUS: st->tcMinus = true; break;
  case H_TC_VALUE: startEdit(st, 1, st->tcRc, formatTC(s.firstClip() + tcOffset(st), s.fps)); return;
  case H_TC_CURSOR: {
    if (st->host->editCursor) {
      double d = st->host->editCursor() - s.firstClip();
      st->tcAbs = std::fabs(d);
      st->tcMinus = d < 0;
    }
    break;
  }
  case H_EV_REMOVE: st->imp->setExcluded(selection(st), true); st->edited = true; break;
  case H_EV_RESTORE: st->imp->setExcluded(selection(st), false); st->edited = true; break;
  case H_EV_MUTE: toggleMute(st); break;
  case H_EV_PLAY: togglePlay(st); break;
  case H_TL_PLAY: toggleTimeline(st); break;
  case H_RULER: {
    double spp = st->viewLen / (kTlr - kTlx);
    double lx = (x - st->ox) / st->scale;
    bool was = st->playTL;
    if (was) stopPlay(st);
    st->playhead = std::max(0.0, st->viewStart + (lx - kTlx) * spp);
    if (was) toggleTimeline(st);
    break;
  }
  case H_EV_UP: case H_EV_DOWN: {
    int d = id == H_EV_UP ? -1 : 1;
    buildRows(st);
    if (!freeDelta(st, d, nullptr) && !st->undo.empty()) st->undo.pop_back(); // nothing changes: no undo step
    moveSelectionBy(st, d);
    break;
  }
  case H_EV_AUTOMATION: st->moveAutomation = !st->moveAutomation; break;
  case H_EV_RESET:
    keepTrackChoices(st, [&]() { st->imp->setMergeStereo(st->merge); });
    for (auto &t : s.tracks)
      for (auto &c : t.clips) c.mute = c.swapLR = false;
    st->sel.clear();
    st->edited = false;
    break;
  case H_CANCEL: closeDialog(st, false); return;
  case H_IMPORT:
    opt.tcOffset = tcOffset(st);
    closeDialog(st, true);
    return;
  default: break;
  }
  InvalidateRect(st->hwnd, NULL, FALSE);
}


// shown track position under device y (rows above / below the window clamp)
int posAtY(State *st, int y) {
  double ly = (y - st->oy) / st->scale;
  int vr = (int)std::floor((ly - kLT) / kRowH);
  return posAtRow(st, st->scrollRow + vr);
}

// keys for the open dialog: + / - zoom; arrows, M and Delete act on the selected events
void stopPlay(State *st);
void stopPlay(State *st) {
  if ((st->playClip || st->playTL) && st->host && st->host->stopPreview) st->host->stopPreview();
  if (st->playTL) st->playhead = st->playFrom; // back to where it started, as in REAPER
  st->gonio.clear();
  st->corr = 0;
  st->playClip = 0;
  st->playTL = false;
  st->playPos = -1;
  if (st->hwnd && st->hwnd != (HWND)1) KillTimer(st->hwnd, kPlayTimer);
}

// listen to the timeline from the playhead: every imported event, mixed
void toggleTimeline(State *st) {
  if (st->playClip || st->playTL) {
    stopPlay(st);
    return;
  }
  if (!st->host || !st->host->playTimeline) return;
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  std::vector<MixEvent> evs;
  double end = 0;
  for (auto &tr : s.tracks) {
    if (!tr.import) continue;
    if (!st->solo.empty() && !st->solo.count(tr.uid)) continue; // solo
    for (auto &c : tr.clips) {
      if (c.excluded || c.mute) continue;
      bool pair = c.sources.size() == 2;
      for (size_t i = 0; i < c.sources.size() && i < 2; i++) {
        const Source &src = c.sources[i];
        if (src.media < 0) continue;
        const Media &m = s.media[(size_t)src.media];
        if (m.foundPath.empty() || !fileExists(m.foundPath)) continue;
        MixEvent e;
        e.path = m.foundPath;
        e.pos = c.pos;
        e.len = c.len;
        e.offset = c.srcOffset;
        e.gain = opt.clipGain ? std::max(0.0, c.gain) : 1.0;
        if (opt.fades) {
          e.fadeIn = c.fadeIn.len;
          e.fadeOut = c.fadeOut.len;
        }
        e.side = pair ? (int)i + 1 : (c.chanMode == 3 ? 1 : c.chanMode == 4 ? 2 : 0);
        if (pair && c.swapLR) e.side = 3 - e.side;
        if (!pair && c.swapLR) e.side = 3;
        evs.push_back(e);
        end = std::max(end, c.pos + c.len);
      }
    }
  }
  if (st->playhead < 0) st->playhead = s.firstClip();
  if (evs.empty() || !st->host->playTimeline(evs, st->playhead, end)) {
    st->tip = "Nothing to play here: the audio isn't found, or REAPER can't read it directly (it is converted on import).";
    return;
  }
  st->playTL = true;
  st->playFrom = st->playhead;
  st->playPos = st->playhead;
  SetTimer(st->hwnd, kPlayTimer, 30, NULL);
}

// listen to the selected event (both files of an L / R pair)
void togglePlay(State *st) {
  if (st->playClip || st->playTL) {
    stopPlay(st);
    return;
  }
  if (st->sel.empty() || !st->host || !st->host->playPreview) return;
  Session &s = st->imp->session();
  Clip *c = st->imp->findClip(*st->sel.begin());
  if (!c) return;
  std::vector<PreviewPart> parts;
  bool pair = c->sources.size() == 2;
  for (size_t i = 0; i < c->sources.size() && i < 2; i++) {
    const Source &src = c->sources[i];
    if (src.media < 0) continue;
    const Media &m = s.media[(size_t)src.media];
    if (m.foundPath.empty() || !fileExists(m.foundPath)) continue;
    PreviewPart p;
    p.path = m.foundPath;
    p.offset = c->srcOffset;
    p.length = c->len;
    p.gain = std::max(0.0, c->gain);
    p.side = pair ? (int)i + 1 : (c->chanMode == 3 ? 1 : c->chanMode == 4 ? 2 : 0);
    if (pair && c->swapLR) p.side = 3 - p.side;
    if (!pair && c->swapLR) p.side = 3; // one stereo file, reversed
    parts.push_back(p);
  }
  if (parts.empty()) {
    st->tip = "This event's audio isn't there (or is embedded in the AAF), so it can't be played here.";
    return;
  }
  if (!st->host->playPreview(parts)) {
    st->tip = "REAPER can't play this file directly. It is converted to Broadcast WAV when you import.";
    return;
  }
  st->playClip = c->id;
  st->playPos = 0;
  gonioStart(st, *c); // stereo: the meter opens
  SetTimer(st->hwnd, kPlayTimer, 30, NULL);
}

bool handleKey(State *st, int vk) {
  if (!st || st->editKind) return false;
  if (vk == VK_TAB) { // the next / previous track
    trackStep(st, (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
    return true;
  }
  if (st->missingOpen && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT)) { // the missing files list
    int step = (vk == VK_PRIOR || vk == VK_NEXT) ? (int)kMissRows - 1 : 1;
    st->missScroll = std::max(0, st->missScroll + ((vk == VK_UP || vk == VK_PRIOR) ? -step : step));
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  bool in = vk == VK_ADD || vk == 0xBB /* VK_OEM_PLUS */;
  bool out = vk == VK_SUBTRACT || vk == 0xBD /* VK_OEM_MINUS */;
#ifndef _WIN32
  in = in || vk == '+' || vk == '=';
  out = out || vk == '-';
#endif
  if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { // as in Pro Tools: T zooms in, R zooms out
    in = in || vk == 'T' || vk == 't';
    out = out || vk == 'R' || vk == 'r';
  }
  if (in || out) {
    zoomBy(st, in ? 0.5 : 2.0, (kTlx + kTlr) / 2);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'H' || vk == VK_F1) {
    st->helpOpen = !st->helpOpen;
    st->optsOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'O') {
    st->optsOpen = !st->optsOpen;
    st->helpOpen = false;
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'Z' && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { // Z: the selected events fill the view, playhead at the start
    zoomToSelection(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if ((vk == 'F' || vk == 'C') && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    if (vk == 'F') revealSelected(st);
    else copySelectedPath(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'F') {
    double a, b;
    sessionRange(st->imp->session(), &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'S' && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) { // Shift + S: solo the selected event's track
    int ti = -1;
    if (!st->sel.empty() && st->imp->findClip(*st->sel.begin(), &ti) && ti >= 0) {
      int uid = st->imp->session().tracks[(size_t)ti].uid;
      bool was = st->solo.count(uid) > 0; // one track at a time
      st->solo.clear();
      if (!was) st->solo.insert(uid);
      if (st->playTL) {
        double at = st->playhead;
        stopPlay(st);
        st->playhead = at;
        toggleTimeline(st);
      }
      InvalidateRect(st->hwnd, NULL, FALSE);
    }
    return true;
  }
  if (vk == 'G') { // the stereo meter
    st->gonioOpen = !st->gonioOpen;
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'C' && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
      !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
    combineSelected(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == 'A' && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) { // A: volume and pan on every track
    bool &on = st->opt->showAutomation;
    on = !on;
    st->tip = on ? "Volume (blue) and pan (purple) automation on every track." : "Automation hidden.";
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if ((vk == 'V' || vk == 'P') && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    // V: volume, P: pan of the selected tracks (by name or by a selected event), else of the track under the mouse.
    // Alt / Option + V or P: of every track. Pressed again: hidden again.
    Session &s = st->imp->session();
    bool all = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    std::set<int> idx;
    if (all)
      for (size_t t = 0; t < s.tracks.size(); t++) idx.insert((int)t);
    else {
      idx = st->selTracks;
      for (size_t t = 0; t < s.tracks.size(); t++)
        for (auto &c : s.tracks[t].clips)
          if (st->sel.count(c.id)) idx.insert((int)t);
      if (idx.empty() && st->mouseLX >= kL && st->mouseLX < kTlr && st->mouseLY >= kLT && st->mouseLY < kLB) {
        int ri = st->scrollRow + (int)std::floor((st->mouseLY - kLT) / kRowH);
        if (ri >= 0 && ri < (int)st->rows.size() && st->rows[(size_t)ri].kind == 0) idx.insert(st->rows[(size_t)ri].track);
      }
    }
    int bit = vk == 'V' ? 1 : 2;
    if (idx.empty()) {
      st->tip = vk == 'V' ? "Select a track or point at it, then V shows its volume (Alt / Option + V: every track)."
                          : "Select a track or point at it, then P shows its pan (Alt / Option + P: every track).";
    } else {
      bool allOn = true;
      for (int t : idx)
        if (t >= 0 && t < (int)s.tracks.size() && !(st->laneShow[laneKey(st, s.tracks[(size_t)t].uid)] & bit)) allOn = false;
      for (int t : idx)
        if (t >= 0 && t < (int)s.tracks.size()) {
          int &m = st->laneShow[laneKey(st, s.tracks[(size_t)t].uid)];
          m = allOn ? (m & ~bit) : (m | bit);
        }
      const char *what = vk == 'V' ? "Volume (blue)" : "Pan (purple)";
      static std::string laneTip;
      laneTip = strprintf("%s %s on %s.", what, allOn ? "hidden" : "shown",
                          all ? "every track" : idx.size() == 1 ? "this track" : "the selected tracks");
      st->tip = laneTip.c_str();
    }
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  // 1-9 on the number row (not the number pad): show that reel
  if (vk >= '1' && vk <= '9' && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
      !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && shownReels(st).size() > 1) {
    std::vector<int> sh = shownReels(st);
    size_t r = (size_t)(vk - '1');
    if (r < sh.size()) switchReel(st, sh[r]);
    else st->tip = "There's no tab with that number.";
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  // Shift + left / right: previous / next reel (the same on every keyboard layout)
  const bool shiftKey = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool prevReel = shiftKey && vk == VK_LEFT, nextReel = shiftKey && vk == VK_RIGHT;
  if ((prevReel || nextReel) && shownReels(st).size() > 1) {
    std::vector<int> sh = shownReels(st);
    size_t k = std::find(sh.begin(), sh.end(), st->reel) - sh.begin();
    if (k >= sh.size()) k = 0;
    switchReel(st, sh[(k + (nextReel ? 1 : sh.size() - 1)) % sh.size()]);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (vk == VK_RETURN) {
    st->opt->tcOffset = tcOffset(st);
    closeDialog(st, true);
    return true;
  }
  if (vk == VK_SPACE) { // Space: the timeline; Shift + Space: the selected event
    bool event = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (st->playClip || st->playTL) stopPlay(st);
    else if (event) togglePlay(st);
    else toggleTimeline(st);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  bool cmd = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; // Cmd on the Mac, Ctrl on Windows
  bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  if (cmd && (vk == 'Z' || vk == 'Y')) {
    undoRedo(st, vk == 'Z' && !shift);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (cmd && vk == 'A') { // select every event (that is not removed)
    st->sel.clear();
    for (auto &tr : st->imp->session().tracks)
      for (auto &c : tr.clips) st->sel.insert(c.id);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (st->sel.empty()) return false;
  buildRows(st);
  bool blockedMove = (vk == VK_UP || vk == VK_DOWN) && !freeDelta(st, vk == VK_UP ? -1 : 1, nullptr);
  if ((vk == VK_UP || vk == VK_DOWN || vk == 'M' || vk == 'm' || vk == VK_DELETE || vk == VK_BACK) && !blockedMove)
    pushUndo(st);
  if (vk == VK_UP) moveSelectionBy(st, -1);
  else if (vk == VK_DOWN) moveSelectionBy(st, 1);
  else if (vk == 'M' || vk == 'm') toggleMute(st);
  else if (vk == VK_DELETE || vk == VK_BACK) toggleExcluded(st);
  else return false;
  InvalidateRect(st->hwnd, NULL, FALSE);
  return true;
}

State *g_open = nullptr; // the dialog that is open (for REAPER's keyboard hook)
HWND g_startHwnd = nullptr; // the start window, while it is open

// Right-click at a point of the window (client coordinates): the track menu on a track name,
// the file menu on an event
void rightClick(State *st, POINT p) {
  if (st->drag) return;
  st->menuTick = GetTickCount();
  HWND hwnd = st->hwnd;
  if (const Hit *hn = hitAt(st, p.x, p.y))
    if (hn->id == H_TRACK_NAME || hn->id == H_TRACK_STEREO) {
      int ti = hn->idx;
      ClientToScreen(hwnd, &p);
      trackMenu(st, p, ti);
      st->menuTick = GetTickCount();
      return;
    }
  const ClipHit *c = clipAt(st, p.x, p.y);
  if (!c) return;
  int id = c->id;
  if (!st->sel.count(id)) { // right-click selects it, like in REAPER
    st->sel.clear();
    st->sel.insert(id);
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
  }
  ClientToScreen(hwnd, &p);
  clipMenu(st, p, id);
  st->menuTick = GetTickCount();
}

INT_PTR CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  State *st = (State *)OL_GETUSER(hwnd);
  if (!st && msg != WM_INITDIALOG) return 0; // before set-up or after the window is gone: the default handling
  switch (msg) {
  case WM_INITDIALOG: {
    st = (State *)lp;
    OL_SETUSER(hwnd, st);
    st->hwnd = hwnd;
    g_open = st;
#ifdef _WIN32
    st->base = st->scale = windowDpi(hwnd) / 96.0; // the monitor the window is on (sharp on every screen)
    RECT want = {0, 0, (LONG)std::lround(W * st->base), (LONG)std::lround(H * st->base)};
    AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT pr;
    HWND par = GetParent(hwnd);
    if (par && GetWindowRect(par, &pr))
      SetWindowPos(hwnd, NULL, std::max((int)pr.left, (int)(pr.left + pr.right - w) / 2),
                   std::max((int)pr.top, (int)(pr.top + pr.bottom - h) / 2), w, h, SWP_NOZORDER);
#endif
    olSetText(hwnd, "OLAAF \xe2\x80\x93 AAF Import for Reaper");
    restoreChoices(st);
    st->reelViews.resize((size_t)reelCount(st));
    // the size it had last time (whole window, as the system measures it), centered on REAPER
    if (st->host && st->host->getSetting) {
      int w = atoi(st->host->getSetting("winW").c_str()), h = atoi(st->host->getSetting("winH").c_str());
      if (w >= 400 && h >= 300 && w < 10000 && h < 10000) {
        RECT pr = {0, 0, 0, 0};
        HWND par = GetParent(hwnd);
        if (par) GetWindowRect(par, &pr);
        int x = par ? (int)(pr.left + pr.right - w) / 2 : 0, y = par ? (int)(pr.top + pr.bottom - h) / 2 : 0;
        SetWindowPos(hwnd, NULL, std::max(0, x), std::max(0, y), w, h, SWP_NOZORDER);
      }
    }
    relayout(st);
    double a, b;
    sessionRange(st->imp->session(), &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
    st->tcAbs = std::fabs(st->opt->tcOffset);
    st->tcMinus = st->opt->tcOffset < 0;
    if (st->host && st->host->getSetting) st->showSamples = st->host->getSetting("samples") == "1";
    st->peaks.start(st->imp->session()); // waveforms fill in as the files are read
    SetTimer(hwnd, kPeakTimer, 150, NULL);
    embedCheck(st); // audio inside the AAF: unpack it for waveforms, or items only?
    if (shownReels(st).size() > 1) askAboutReels(st); // several AAFs chosen at once
    SetTimer(hwnd, kUpdTimer, 1000, NULL); // is there a newer OLAAF?
    return 0;
  }
  case WM_TIMER:
    if (st && wp == kTipTimer) {
      KillTimer(hwnd, kTipTimer);
      if (st->tip) {
        st->tipVisible = true;
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    if (st && wp == kBlinkTimer) {
      st->blinkOn = !st->blinkOn;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st && wp == kUpdTimer) { // the update check runs in the background; stop asking after a while
      std::string tag, url;
      int u = st->host && st->host->updateState ? st->host->updateState(&tag, &url) : 2;
      if (u == 1) {
        st->updTag = tag;
        st->updUrl = url;
        SetTimer(hwnd, kBlinkTimer, 700, NULL);
        InvalidateRect(hwnd, NULL, FALSE);
      }
      if (u != 0 || ++st->updPolls > 20) KillTimer(hwnd, kUpdTimer);
      return 0;
    }
    if (st && wp == kPlayTimer) {
      double p = st->host && st->host->previewPos ? st->host->previewPos() : -1;
      if (p < 0) stopPlay(st);
      else {
        st->playPos = p;
        if (st->playClip) gonioUpdate(st);
        if (st->playTL) { // the playhead moves; the view follows when it runs off the right
          st->playhead = p;
          if (p > st->viewStart + st->viewLen * 0.95) st->viewStart = p - st->viewLen * 0.05;
        }
      }
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st && wp == kPeakTimer) {
      if (st->embedJob) {
        embedPoll(st);
        InvalidateRect(hwnd, NULL, FALSE);
      }
      if (st->peaks.version() != st->peakSeen) InvalidateRect(hwnd, NULL, FALSE);
      if (!st->embedJob && !st->peaks.busy() && st->peaks.version() == st->peakSeen) KillTimer(hwnd, kPeakTimer);
    }
    return 0;
  case WM_ERASEBKGND: return 1;
#ifdef _WIN32
  case 0x02E0: // WM_DPICHANGED: moved to a screen with another scaling - take the size Windows suggests, redraw sharp
    if (st) {
      st->base = HIWORD(wp) / 96.0;
      const RECT *r = (const RECT *)lp;
      SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      relayout(st);
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
#endif
  case WM_SIZE:
    if (st) {
      if (st->editKind) endEdit(st, true);
      relayout(st);
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  case WM_GETMINMAXINFO: {
    MINMAXINFO *mm = (MINMAXINFO *)lp;
    double b = st ? st->base : 1.0;
    mm->ptMinTrackSize.x = (LONG)(W * b * 0.6);
    mm->ptMinTrackSize.y = (LONG)(H * b * 0.6);
    return 0;
  }
#ifdef _WIN32
  case WM_GETDLGCODE: // the dialog wants the arrow keys and letters for the selected events
    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB);
    return TRUE;
#endif
  case WM_SYSKEYDOWN: // Alt / Option + V or P
    if (st && (wp == 'V' || wp == 'P') && handleKey(st, (int)wp)) return 1;
    break;
  case WM_KEYDOWN:
    if (st && handleKey(st, (int)wp)) return 1;
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
#ifdef _WIN32
    RECT cr;
    GetClientRect(hwnd, &cr);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(st, mem);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
#else
    paint(st, dc);
#endif
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_LBUTTONDBLCLK: // the second click of a double-click (Windows / macOS send it instead of a down)
  case WM_LBUTTONDOWN: {
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    if (st->reelAsk || st->embedAsk) { // a question waits for its answer: no drags, no selections underneath
      st->drag = 0; // the buttons answer on release (click)
      return 0;
    }
    if (st->editKind && !PtInRect(&st->editRc, p)) endEdit(st, true); // clicking elsewhere applies what was typed
    if (!st->editKind) SetFocus(hwnd); // so the arrow keys reach the selected events
    st->dragMoved = false;
    st->pressedClip = 0;
    int nrows = (int)st->rows.size();
    bool popOpen = st->palTrack >= 0 || !st->palFolder.empty() || st->folderTrack >= 0 || st->destTrack >= 0;
    if (PtInRect(&st->vTrack, p) && nrows > kRows) {
      if (PtInRect(&st->vThumb, p)) {
        st->drag = 1;
        st->dragFromY = p.y;
        st->dragValue = st->scrollRow;
      } else
        st->scrollRow += p.y < st->vThumb.top ? -kRows : kRows;
      SetCapture(hwnd);
    } else if (PtInRect(&st->hTrack, p)) {
      if (PtInRect(&st->hThumb, p)) {
        st->drag = 2;
        st->dragFromX = p.x;
        st->dragValue = st->viewStart;
      } else
        st->viewStart += p.x < st->hThumb.left ? -st->viewLen * 0.9 : st->viewLen * 0.9;
      SetCapture(hwnd);
    } else if (!popOpen && hitAt(st, p.x, p.y) && hitAt(st, p.x, p.y)->id == H_TRACK_NAME) {
      // track names: select now (or on release with a modifier) and maybe drag into a folder
      // selected name: drag moves the selection; unselected name: drag selects a range
      int t = hitAt(st, p.x, p.y)->idx;
      st->nameRc = hitAt(st, p.x, p.y)->r;
      st->pressedTrack = -1;
      if (shiftDown() || ctrlDown() || st->selTracks.count(t)) {
        st->pressedTrack = t;
        st->drag = st->selTracks.count(t) && !shiftDown() && !ctrlDown() ? 4 : 0;
      } else {
        st->selTracks.clear();
        st->selTracks.insert(t);
        st->anchorTrack = t;
        st->drag = 5;
        st->rangeFromRow = rowAtY(st, p.y);
      }
      if (shiftDown() || ctrlDown()) st->drag = 6; // decided on release
      st->dragFromY = p.y;
      st->dragMouseY = p.y;
      st->dropRow = -1;
      SetCapture(hwnd);
    } else if (!popOpen) {
      if (const ClipHit *c = clipAt(st, p.x, p.y)) {
        // pressing an unselected event selects it, so it can be dragged right away
        if (!st->sel.count(c->id) && !shiftDown()) {
          st->sel.clear();
          st->sel.insert(c->id);
        } else
          st->pressedClip = c->id; // shift-click or click on a selected event: decided on release
        int t = -1;
        st->imp->findClip(c->id, &t);
        st->dragFromPos = posOfTrack(st, t);
        st->drag = 3;
        st->dragFromY = p.y;
        st->dragRows = 0;
        SetCapture(hwnd);
      } else if (const Hit *hl = hitAt(st, p.x, p.y)) {
        if (hl->id == H_LANES) { // empty lane: drag a frame to select events (click alone: playhead)
          st->drag = 7;
          st->mqX0 = st->mqX1 = p.x;
          st->mqY0 = st->mqY1 = p.y;
          st->mqBase = shiftDown() ? st->sel : std::set<int>();
          SetCapture(hwnd);
        }
      }
    }
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_LBUTTONUP: {
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    int drag = st->drag;
    bool moved = st->dragMoved;
    if (GetCapture() == hwnd) ReleaseCapture();
    st->drag = 0;
    st->dragMoved = false;
    if (drag == 4 || drag == 5 || drag == 6) {
      // a double-click on a name (without moving) renames the track
      int hitTrack = -1;
      if (const Hit *hh = hitAt(st, p.x, p.y))
        if (hh->id == H_TRACK_NAME) hitTrack = hh->idx;
      unsigned now = GetTickCount();
      if (!moved && hitTrack >= 0 && hitTrack == st->lastNameTrack && now - st->lastNameClick < 450 && !shiftDown() &&
          !ctrlDown()) {
        st->lastNameTrack = -1;
        st->selTracks.clear();
        st->selTracks.insert(hitTrack);
        st->editTrack = hitTrack;
        startEdit(st, 4, st->nameRc, st->imp->session().tracks[(size_t)hitTrack].name);
        st->pressedTrack = -1;
        return 0;
      }
      if (!moved) {
        st->lastNameClick = now;
        st->lastNameTrack = hitTrack;
      }
      if (drag == 4 && moved && st->dropRow >= 0) {
        pushUndo(st);
        dropTracks(st, st->dropRow);
      }
      else if (drag != 5 && st->pressedTrack >= 0) {
        int t = st->pressedTrack;
        if (shiftDown()) { // Shift: everything from the last clicked track to this one (as shown)
          int a = posOfTrack(st, st->anchorTrack >= 0 ? st->anchorTrack : t), b = posOfTrack(st, t);
          if (a < 0) a = b;
          st->selTracks.clear();
          for (int i = std::min(a, b); i <= std::max(a, b) && i < (int)st->order.size(); i++)
            st->selTracks.insert(st->order[(size_t)i]);
        } else if (ctrlDown()) { // Cmd / Ctrl: add or take away one
          if (st->selTracks.count(t)) st->selTracks.erase(t);
          else st->selTracks.insert(t);
          st->anchorTrack = t;
        } else {
          st->selTracks.clear();
          st->selTracks.insert(t);
          st->anchorTrack = t;
        }
      }
      st->dropRow = -1;
      st->pressedTrack = -1;
      st->tip = nullptr;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (drag == 7) {
      st->marquee = false;
      if (!moved) click(st, p.x, p.y, shiftDown());
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (drag == 3) {
      if (moved && st->dragRows) {
        if (freeDelta(st, st->dragRows, nullptr)) pushUndo(st);
        moveSelectionBy(st, st->dragRows); // a taken spot: the next free track, the footer says so
      }
      else if (st->pressedClip) click(st, p.x, p.y, shiftDown());
      bool keepTip = st->tip && strstr(st->tip, " is taken by ");
      st->dragRows = 0;
      st->dragBlocked = false;
      if (!keepTip) st->tip = nullptr;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    bool onBar = PtInRect(&st->vTrack, p) || PtInRect(&st->hTrack, p);
    if (!drag && !onBar) click(st, p.x, p.y, shiftDown());
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_MOUSEMOVE: {
    if (st && st->drag == 7) {
      double mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
      if (std::fabs(mx - st->mqX0) + std::fabs(my - st->mqY0) > 5 * st->scale) st->dragMoved = true;
      if (st->dragMoved) {
        st->marquee = true;
        st->mqX1 = mx;
        st->mqY1 = my;
        RECT q = {(LONG)std::min(st->mqX0, mx), (LONG)std::min(st->mqY0, my), (LONG)std::max(st->mqX0, mx),
                  (LONG)std::max(st->mqY0, my)};
        st->sel = st->mqBase;
        for (auto &ch : st->clipHits) {
          RECT x;
          if (IntersectRect(&x, &q, &ch.r)) st->sel.insert(ch.id);
        }
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    if (st->drag == 1) {
      double span = (double)(st->vTrack.bottom - st->vTrack.top) - (double)(st->vThumb.bottom - st->vThumb.top);
      int range = (int)st->rows.size() - kRows;
      if (span > 0 && range > 0) st->scrollRow = (int)std::lround(st->dragValue + (my - st->dragFromY) * range / span);
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st->drag == 2) {
      double span = (double)(st->hTrack.right - st->hTrack.left) - (double)(st->hThumb.right - st->hThumb.left);
      double range = (st->rangeB - st->rangeA) - st->viewLen;
      if (span > 0 && range > 0) st->viewStart = st->dragValue + (mx - st->dragFromX) * range / span;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st->drag == 5) { // select every track between the pressed name and here
      double ly = (my - st->oy) / st->scale;
      if (ly < kLT + 4 && st->scrollRow > 0) st->scrollRow--;
      if (ly > kLB - 4 && st->scrollRow + kRows < (int)st->rows.size()) st->scrollRow++;
      int r = rowAtY(st, my);
      if (r < 0) r = ly < kLT ? st->scrollRow : std::min((int)st->rows.size() - 1, st->scrollRow + kRows - 1);
      int a = std::min(st->rangeFromRow, r), b = std::max(st->rangeFromRow, r);
      st->selTracks.clear();
      for (int i = std::max(0, a); i <= b && i < (int)st->rows.size(); i++)
        if (st->rows[(size_t)i].kind == 0) st->selTracks.insert(st->rows[(size_t)i].track);
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st->drag == 4) {
      if (std::abs(my - st->dragFromY) > (int)std::lround(5 * st->scale)) st->dragMoved = true;
      if (st->dragMoved) {
        st->dragMouseY = my;
        st->dropRow = slotAtY(st, my);
        double ly = (my - st->oy) / st->scale;
        if (ly < kLT + 4 && st->scrollRow > 0) st->scrollRow--;                       // scroll while dragging
        if (ly > kLB - 4 && st->scrollRow + kRows < (int)st->rows.size()) st->scrollRow++;
        std::string key = dropKey(st, st->dropRow);
        Folder *f = key.empty() ? nullptr : st->imp->session().folder(key);
        static std::string tipBuf;
        tipBuf = f ? "Release to put the selected tracks into " + f->name + "." : std::string("Release to put the selected tracks here, outside any folder.");
        st->tip = tipBuf.c_str();
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    if (st->drag == 3) {
      int dy = my - st->dragFromY;
      if (std::abs(dy) > (int)std::lround(5 * st->scale)) st->dragMoved = true;
      if (st->dragMoved) {
        int target = posAtY(st, my);
        st->dragRows = std::max(target, 0) - st->dragFromPos;
        static std::string dragTip;
        st->dragEff = freeDelta(st, st->dragRows, &dragTip);
        st->dragBlocked = st->dragRows != 0 && st->dragEff == 0;
        if (!dragTip.empty()) { // "went to" while still dragging: "goes to"
          size_t w = dragTip.find(" went to ");
          if (w != std::string::npos) dragTip.replace(w, 9, " goes to ");
        }
        st->tip = !dragTip.empty() ? dragTip.c_str()
                                   : "Release to move the selected events to this track. Below the last track a new one is added.";
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    const Hit *h = hitAt(st, mx, my);
    const char *tip = h ? h->tip : nullptr;
    double lx = (mx - st->ox) / st->scale;
    int hr = (lx >= kL && lx < kL + kHdr && !st->optsOpen) ? rowAtY(st, my) : -1;
    st->mouseLX = (mx - st->ox) / st->scale;
    st->mouseLY = (my - st->oy) / st->scale;
    // the hint is copied: the text it points to is rebuilt with every paint (a pointer to it went stale
    // and showed random characters in the footer)
    bool same = tip ? (st->tip == st->tipHold.c_str() && st->tipHold == tip) : st->tip == nullptr;
    if (!same || hr != st->hoverRow) {
      if (tip) {
        st->tipHold = tip;
        st->tip = st->tipHold.c_str();
      } else
        st->tip = nullptr;
      st->hoverRow = hr;
      st->tipVisible = false;
      KillTimer(hwnd, kTipTimer);
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  }
  case WM_MOUSEHWHEEL: {
    int delta = (short)HIWORD(wp);
    st->viewStart += st->viewLen * 0.05 * (delta > 0 ? 1 : -1);
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_RBUTTONUP: { // the event's file / the track: their menus
    if (st && (st->reelAsk || st->embedAsk)) return 0;
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    rightClick(st, p);
    return 0;
  }
  case WM_CONTEXTMENU: { // some systems send this instead (or as well): the same menus, once
    if (st && (st->reelAsk || st->embedAsk)) return 0;
    if (GetTickCount() - st->menuTick < 600) return 0;
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    if (p.x == -1 && p.y == -1) return 0; // from the keyboard: no place to point at
    ScreenToClient(hwnd, &p);
    rightClick(st, p);
    return 0;
  }
  case WM_MOUSEWHEEL: {
    int delta = (short)HIWORD(wp);
    if (st->missingOpen) { // scrolls the list of missing files
      st->missScroll += delta > 0 ? -3 : 3;
      if (st->missScroll < 0) st->missScroll = 0; // the top end is kept when painting
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (st->destTrack >= 0) { // scrolls the track list of the "put on" popover
      st->destScroll += delta > 0 ? -1 : 1;
      if (st->destScroll < 0) st->destScroll = 0;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (altDown()) { // Alt / Option + wheel: zoom around the mouse
      POINT mp = {(short)LOWORD(lp), (short)HIWORD(lp)};
      ScreenToClient(hwnd, &mp);
      zoomBy(st, delta > 0 ? 0.8 : 1.25, (mp.x - st->ox) / st->scale);
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    if (shiftDown()) st->viewStart -= st->viewLen * 0.1 * (delta > 0 ? 1 : -1);
    else st->scrollRow += delta > 0 ? -1 : 1;
    if (st->scrollRow < 0) st->scrollRow = 0;
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_TCEDIT) {
      if (HIWORD(wp) == EN_KILLFOCUS) endEdit(st, true);
      return 0;
    }
    if (LOWORD(wp) == IDOK) {
      if (st->editKind) endEdit(st, true); // Return applies what was typed
      return 0;
    }
    if (LOWORD(wp) == IDCANCEL) {
      if (st->editKind) endEdit(st, false); // Esc leaves the field, not the dialog
      else if (st->folderTrack >= 0 || st->palTrack >= 0 || !st->palFolder.empty() || st->destTrack >= 0) {
        st->folderTrack = st->palTrack = st->destTrack = -1;
        st->palFolder.clear();
        InvalidateRect(hwnd, NULL, FALSE);
      } else
        closeDialog(st, false);
    }
    return 0;
#ifdef _WIN32
  case WM_CTLCOLOREDIT: {
    if (st->editBrush) DeleteObject(st->editBrush);
    bool counter = st->editKind == 1;
    st->editBrush = CreateSolidBrush(counter ? kCounterBg : kPanel);
    SetTextColor((HDC)wp, counter ? kCounterInk : kInk);
    SetBkColor((HDC)wp, counter ? kCounterBg : kPanel);
    return (INT_PTR)st->editBrush;
  }
#endif
  case WM_CLOSE:
    if (st) closeDialog(st, false);
    return 0;
  case WM_DESTROY:
    if (st && st->host && st->host->setSetting) { // remember the size for next time
      RECT wr;
      if (GetWindowRect(hwnd, &wr) && wr.right - wr.left >= 400) {
        st->host->setSetting("winW", std::to_string(wr.right - wr.left));
        st->host->setSetting("winH", std::to_string(std::abs(wr.bottom - wr.top)));
      }
    }
    if (g_open == st) g_open = nullptr;
    KillTimer(hwnd, kPeakTimer);
    KillTimer(hwnd, kTipTimer);
    KillTimer(hwnd, kUpdTimer);
    KillTimer(hwnd, kBlinkTimer);
    if (st) stopPlay(st);
    if (st) embedCancel(st, nullptr); // waits for the file being unpacked; the import uses the AAF next
    if (st) st->peaks.stop();
    if (st) {
      freeFonts(st);
      if (st->editBrush) DeleteObject(st->editBrush);
    }
    if (st && st->modeless) {
      OL_SETUSER(hwnd, 0);
      delete st;
    }
    return 0;
  }
  return 0;
}

#ifdef _WIN32
struct DlgTemplate {
  DLGTEMPLATE t;
  WORD menu, cls;
  WCHAR title[8];
};
#endif

} // namespace

void relayoutStart(State *st);
void paintStart(State *st, HDC dc);
#ifdef OLAAF_DIALOG_TEST
// renders the dialog into any HDC, for checking the layout without REAPER
// palTrack: >= 0 color popover of that track, <= -2 folder popover of track (-2 - n)
void dialogPaintTest(Importer &imp, ImportOptions &opt, HDC dc, int palTrack, const char *tip) {
  static DialogHost host;
  static Importer *firstImp;
  static ImportOptions *firstOpt;
  firstImp = &imp;
  firstOpt = &opt;
  if (getenv("OLAAF_START")) { // the start window
    static StartData sd;
    sd.recent.clear();
    if (atoi(getenv("OLAAF_START")) == 2)
      sd.recent = {imp.session().aafPath, "/Volumes/Projects/Pathologie/AAF/Pathologie_Reel2_v3.aaf",
                   "/Users/bernd/Desktop/Namib_Edit_V1.aaf"};
    State ss;
    ss.start = &sd;
    ss.hwnd = (HWND)1;
    ss.tip = tip;
    if (getenv("OLAAF_STARTHOVER")) ss.startHover = 0;
    if (getenv("OLAAF_UPD")) ss.updTag = "v0.8.1";
    relayoutStart(&ss);
    paintStart(&ss, dc);
    freeFonts(&ss);
    return;
  }
  State st;
  st.imp = &imp;
  st.opt = &opt;
  st.host = &host;
  st.hwnd = (HWND)1;
  st.merge = opt.mergeStereo;
  st.tip = tip;
  st.peaks.runNow(imp.session());
  relayout(&st); // zoomed to the (test) window
  double a, b;
  sessionRange(imp.session(), &a, &b);
  st.viewStart = a - (b - a) * 0.02;
  st.viewLen = (b - a) * 1.04;
  st.tcAbs = 0;
  auto &tr = imp.session().tracks;
  if (!tr.empty() && tr[0].clips.size() > 2) {
    st.sel.insert(tr[0].clips[2].id);
    imp.setExcluded({tr[0].clips[1].id}, true);
    if (tr[0].clips.size() > 3) tr[0].clips[3].mute = true;
    st.edited = true;
  }
  if (getenv("OLAAF_SHOWAUTO")) opt.showAutomation = true;
  if (getenv("OLAAF_OPTS")) st.optsOpen = true;
  if (getenv("OLAAF_HELP")) st.helpOpen = true;
  if (getenv("OLAAF_MISSING")) {
    st.missingOpen = true;
    st.missScroll = atoi(getenv("OLAAF_MISSING"));
  }
  if (getenv("OLAAF_RULESTEXT")) setSortRules(sortRulesFromText(getenv("OLAAF_RULESTEXT")));
  if (const char *gn = getenv("OLAAF_NAMES")) { // test: other track names, "|" between them
    std::string all = gn;
    size_t i = 0, a = 0;
    while (i < st.imp->session().tracks.size() && a <= all.size()) {
      size_t e = all.find('|', a);
      if (e == std::string::npos) e = all.size();
      st.imp->session().tracks[i++].name = all.substr(a, e - a);
      a = e + 1;
    }
  }
  if (const char *so = getenv("OLAAF_SORTED")) {
    opt.subFolders = atoi(so) == 2;
    st.imp->assignDefaultFolders(opt.subFolders);
  }
  if (getenv("OLAAF_EMBEDASK")) st.embedAsk = true;
  if (getenv("OLAAF_RULES")) st.rulesOpen = true;
  if (getenv("OLAAF_CLOSEFX")) {
    st.closedFolders.insert("FX");
    buildRows(&st);
  }
  if (getenv("OLAAF_TCOFF")) st.tcAbs = atof(getenv("OLAAF_TCOFF"));
  if (getenv("OLAAF_SPLIT")) { // the first combined track split again
    for (size_t i = 0; i < imp.session().tracks.size(); i++)
      if (!imp.session().tracks[i].mergedFrom.empty()) {
        imp.splitStereo((int)i, nullptr);
        break;
      }
    buildRows(&st);
  }
  if (getenv("OLAAF_VOLONLY")) st.laneShow[laneKey(&st, st.imp->session().tracks[0].uid)] = 1;
  if (getenv("OLAAF_PAIR")) { // two mono tracks not named .L / .R, selected (OLAAF_PAIR=2: combined with C)
    st.imp->setMergeStereo(false);
    Session &ps = st.imp->session();
    if (ps.tracks.size() > 4) {
      ps.tracks[3].name = ps.tracks[3].aafName = "Boom 1";
      ps.tracks[4].name = ps.tracks[4].aafName = "Boom 2";
    }
    buildRows(&st);
    st.selTracks = {3, 4};
    if (atoi(getenv("OLAAF_PAIR")) == 2) combineSelected(&st);
  }
  if (getenv("OLAAF_PANSEL")) { st.laneShow[laneKey(&st, st.imp->session().tracks[0].uid)] = 2; st.selTracks = {0}; }
  if (getenv("OLAAF_RENAME")) { // the first two tracks renamed
    imp.session().tracks[0].name = "Dialog";
    if (imp.session().tracks.size() > 2) imp.session().tracks[2].name = "Atmo Wind";
  }
  if (getenv("OLAAF_RMEMPTY")) {
    static std::string done;
    std::vector<std::string> names;
    int n = imp.removeEmptyTracks(&names);
    done = strprintf("%d empty track%s deleted: %s. Cmd/Ctrl+Z brings %s back.", n, n == 1 ? "" : "s",
                     names.empty() ? "" : names[0].c_str(), n == 1 ? "it" : "them");
    st.tip = done.c_str();
  }
  if (getenv("OLAAF_PEAKBUSY")) st.peaks.fakeProgress(atoi(getenv("OLAAF_PEAKBUSY")), 76);
  if (getenv("OLAAF_CHOICES")) st.choicesBack = true;
  if (getenv("OLAAF_TPL")) st.opt->trackTemplate = getenv("OLAAF_TPL");
  if (getenv("OLAAF_THEMEMENU")) st.themeMenu = true;
  host.addReels = [](HWND, std::string *) { return 0; };
  static std::vector<std::unique_ptr<Importer>> moreImp;
  static std::vector<std::unique_ptr<ImportOptions>> moreOpt;
  if (const char *rl = getenv("OLAAF_REELS")) { // "Name 1,Name 2,...": the same AAF as several reels
    std::vector<std::string> names;
    for (const char *p = rl; *p;) {
      const char *e = strchr(p, ',');
      names.push_back(e ? std::string(p, e) : std::string(p));
      if (!e) break;
      p = e + 1;
    }
    std::string dir = dirName(imp.session().aafPath), src = imp.session().aafPath;
    for (size_t i = 0; i < names.size(); i++) {
      if (i == 0) {
        imp.session().aafPath = joinPath(dir, names[0] + ".aaf");
        continue;
      }
      std::unique_ptr<Importer> ri(new Importer);
      std::unique_ptr<ImportOptions> ro(new ImportOptions(opt));
      std::string e;
      ri->load(src, *ro, &e);
      ri->locateMedia(*ro);
      ri->setMergeStereo(true);
      ri->session().aafPath = joinPath(dir, names[i] + ".aaf");
      ro->tcOffset = getenv("OLAAF_REELOVER") ? 0 : 3600.0 * (double)i;
      moreImp.push_back(std::move(ri));
      moreOpt.push_back(std::move(ro));
    }
    host.reelCount = []() { return 1 + (int)moreImp.size(); };
    static bool hide0 = getenv("OLAAF_HIDE0") != nullptr;
    host.reelHidden = [](int i) { return i == 0 && hide0; };
    if (hide0) st.reel = 0;
    host.reelAt = [](int i, Importer **ri, ImportOptions **ro) {
      if (i == 0) {
        *ri = firstImp;
        *ro = firstOpt;
        return true;
      }
      if (i < 1 || i - 1 >= (int)moreImp.size()) return false;
      *ri = moreImp[(size_t)i - 1].get();
      *ro = moreOpt[(size_t)i - 1].get();
      return true;
    };
    st.reelViews.resize(1 + moreImp.size());
    if (getenv("OLAAF_REELASK")) { // the question after adding AAFs
      std::vector<Session> ss;
      std::vector<std::string> names2;
      for (int i = 0; i < 1 + (int)moreImp.size(); i++) {
        Importer *ri;
        ImportOptions *ro;
        host.reelAt(i, &ri, &ro);
        ss.push_back(ri->session());
        names2.push_back(reelLabel(ri->session(), i));
      }
      std::vector<std::string> nl = reelNumberLabels(names2);
      if (nl.empty()) nl = reelShortLabels(names2);
      for (size_t k = 0; k < nl.size(); k++) st.reelAskNames += (k == 0 ? "" : k + 1 == nl.size() ? " and " : ", ") + nl[k];
      st.reelAskRelated = reelsBelongTogether(ss, &st.reelAskWhy);
      st.reelAsk = true;
    }
    if (getenv("OLAAF_OWN")) opt.reelsShareTracks = false;
  }
  if (getenv("OLAAF_LONGNAME") && imp.session().tracks.size() > 1) {
    imp.session().tracks[0].name = "Dialog_Boom_Szene_12_Take_3_Close_Mic_Rechts";
    imp.session().tracks[1].name = "Lavalier_Anna_Szene_12_alle_Takes_mit_Wind";
  }
  if (getenv("OLAAF_SOLO"))
    for (auto &tr : imp.session().tracks)
      if (tr.name == getenv("OLAAF_SOLO")) st.solo.insert(tr.uid);
  if (getenv("OLAAF_VIEW")) sscanf(getenv("OLAAF_VIEW"), "%lf,%lf", &st.viewStart, &st.viewLen);
  if (const char *dr = getenv("OLAAF_DRAG")) { // the selection dragged by n tracks
    buildRows(&st);
    int t = -1;
    if (!st.sel.empty() && imp.findClip(*st.sel.begin(), &t)) {
      st.drag = 3;
      st.dragMoved = true;
      st.dragFromPos = posOfTrack(&st, t);
      st.dragRows = atoi(dr);
      static std::string why;
      st.dragEff = freeDelta(&st, st.dragRows, &why);
      st.dragBlocked = st.dragEff == 0;
      st.tip = !why.empty() ? why.c_str() : "Release to move the selected events to this track.";
    }
  }
  if (getenv("OLAAF_GONIO")) // the MUSIC pair, 20 s in
    for (auto &tr : imp.session().tracks)
      for (auto &c : tr.clips)
        if (!st.gonioOpen && c.sources.size() == 2) {
          if (getenv("OLAAF_SWAP")) c.swapLR = true;
          gonioStart(&st, c);
          st.gonioOpen = true;
          st.playClip = c.id;
          st.sel.clear();
          st.sel.insert(c.id);
          double p0 = getenv("OLAAF_GPOS") ? atof(getenv("OLAAF_GPOS")) : 20;
          for (int k = 0; k < 8; k++) {
            st.playPos = p0 + k * 0.03;
            gonioUpdate(&st);
          }
        }
  if (getenv("OLAAF_UPD")) st.updTag = "v0.8.1";
  if (getenv("OLAAF_TIP")) {
    st.tip = "Where the AAF lands in REAPER. In the AAF it starts at 00:00:00:00. Click and type HH:MM:SS:FF or just the digits; Return applies, Esc cancels.";
    st.tipVisible = true;
    st.mouseLX = 640;
    st.mouseLY = 40;
  }
  host.playTimeline = [](const std::vector<MixEvent> &, double, double) { return false; };
  host.playPreview = [](const std::vector<PreviewPart> &) { return false; };
  st.playhead = imp.session().firstClip() + 9.5;
  if (getenv("OLAAF_ZOOMSEL")) {
    relayout(&st);
    zoomToSelection(&st);
  }
  if (getenv("OLAAF_FPS")) host.projectFps = [](bool *d) { *d = false; return 24.0; };
  if (getenv("OLAAF_SR")) host.projectSrate = []() { return 44100.0; };
  if (getenv("OLAAF_HOVER")) st.hoverRow = atoi(getenv("OLAAF_HOVER"));
  if (getenv("OLAAF_DEST")) { // the "put on" popover with a made-up project
    host.projectTracks = []() {
      return std::vector<std::pair<std::string, std::string>>{{"{1}", "DX Boom"}, {"{2}", "DX Lav"}, {"{3}", "MIX"},
                                                             {"{4}", "Music"}, {"{5}", "Atmo"}};
    };
    st.projTracks = host.projectTracks();
    imp.session().tracks[2].target = "{3}";
    imp.session().tracks[2].targetName = "MIX";
    st.destTrack = 3;
    st.dpX = 60;
    st.dpY = 300;
    st.dpY0 = 270;
  }
  if (palTrack >= 0) {
    st.palTrack = palTrack;
    st.palX = 32;
    st.palY = 300;
    st.palY0 = 270;
  } else if (palTrack == -9) { // dragging two track names onto the first folder
    st.selTracks = {2, 3};
    st.drag = 4;
    st.dragMoved = true;
    buildRows(&st);
    st.dropRow = 1;
    st.dragMouseY = 175;
  } else if (palTrack <= -2) {
    st.folderTrack = -2 - palTrack;
    st.fpX = 60;
    st.fpY = 240;
    st.fpY0 = 210;
  }
  if (const char *bn = getenv("OLAAF_BENCH")) { // how long one repaint takes
    auto t0 = std::chrono::steady_clock::now();
    int n = std::max(1, atoi(bn));
    for (int i = 0; i < n; i++) paint(&st, dc);
    fprintf(stderr, "paint: %.2f ms each\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n);
  }
  paint(&st, dc);
  if (const char *hp = getenv("OLAAF_HITAT")) { // which hit answers a click at x,y (logical px)
    int x = 0, y = 0;
    sscanf(hp, "%d,%d", &x, &y);
    const Hit *h = hitAt(&st, (int)(st.ox + x * st.scale), (int)(st.oy + y * st.scale));
    fprintf(stderr, "hit at %d,%d: id %d idx %d\n", x, y, h ? (int)h->id : -1, h ? h->idx : -1);
  }
}
#endif

// REAPER hands every key press to registered extensions first (otherwise its own shortcuts,
// like M, take them): returns 1 when the dialog used the key, -1 to let the dialog have it, 0 if not ours
int dialogTranslateKey(MSG *msg) {
  if (g_startHwnd && msg && (msg->hwnd == g_startHwnd || IsChild(g_startHwnd, msg->hwnd)))
    return -1; // the start window takes its keys itself (Return, Esc), not REAPER
  State *st = g_open;
  if (!st || !st->hwnd || !msg) return 0;
  if (msg->hwnd != st->hwnd && !IsChild(st->hwnd, msg->hwnd)) return 0;
  if (st->editKind) { // typing in a field: Return applies, Esc cancels, Tab goes on to the next track name
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB && st->editKind == 4) {
      trackStep(st, (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
      return 1;
    }
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB && st->editKind == 5) { // the next folder's words
      int i = atoi(st->editKey.c_str()) + ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
      endEdit(st, true);
      UpdateWindow(st->hwnd);
      for (auto &hh : st->hits)
        if (hh.id == H_RULE_WORDS && hh.idx == i) {
          std::vector<SortRule> rules = sortRules();
          std::string v;
          for (size_t k = 0; k < rules[(size_t)i].words.size(); k++) v += (k ? ", " : "") + rules[(size_t)i].words[k];
          startEdit(st, 5, hh.r, v, std::to_string(i));
          break;
        }
      return 1;
    }
    if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE)) {
      endEdit(st, msg->wParam == VK_RETURN);
      return 1;
    }
    return -1;
  }
  if (!st->reelAsk && st->embedAsk && msg->message == WM_KEYDOWN) { // Return: load waveforms, Esc: items only
    if (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE) {
      embedAnswer(st, msg->wParam == VK_RETURN);
      InvalidateRect(st->hwnd, NULL, FALSE);
    }
    return 1;
  }
  if (st->reelAsk && msg->message == WM_KEYDOWN) { // the reel question: Return / Esc take the suggested answer
    if (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE) {
      reelAnswer(st, st->reelAskRelated);
      InvalidateRect(st->hwnd, NULL, FALSE);
    }
    return 1;
  }
  if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
    if (st->helpOpen || st->optsOpen || st->missingOpen || st->rulesOpen || st->themeMenu || st->folderTrack >= 0 || st->destTrack >= 0 || st->palTrack >= 0 || !st->palFolder.empty()) {
      st->optsOpen = st->helpOpen = st->missingOpen = st->rulesOpen = st->themeMenu = false; // Esc closes an open panel first
      st->folderTrack = st->destTrack = st->palTrack = -1;
      st->palFolder.clear();
      InvalidateRect(st->hwnd, NULL, FALSE);
      return 1;
    }
    SendMessage(st->hwnd, WM_COMMAND, IDCANCEL, 0);
    return 1;
  }
  // Alt / Option + V or P arrive as system keys on Windows
  bool altVP = msg->message == WM_SYSKEYDOWN && (msg->wParam == 'V' || msg->wParam == 'P');
  if ((msg->message == WM_KEYDOWN || altVP) && handleKey(st, (int)msg->wParam)) return 1;
  if (msg->message == WM_KEYDOWN || msg->message == WM_CHAR || msg->message == WM_KEYUP) return -1;
  return 0;
}

// ------------------------------------------------------------------ the start window
// Shown when OLAAF opens: what it does, in three steps, which file type, the AAFs opened before,
// and one button to choose the file.

const double SW = 760, SH = 540;

void relayoutStart(State *st) {
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  double zoom = std::max(0.5, std::min(cr.right / (SW * st->base), cr.bottom / (SH * st->base)));
  double scale = st->base * zoom;
  st->ox = std::max(0.0, std::floor((cr.right - SW * scale) / 2));
  st->oy = std::max(0.0, std::floor((cr.bottom - SH * scale) / 2));
  if (std::fabs(scale - st->scale) > 1e-6 || !st->fBody) {
    st->scale = scale;
    freeFonts(st);
    makeFonts(st);
  }
}

void paintStart(State *st, HDC dc) {
  Gfx g{dc, st->scale, st->ox, st->oy};
  st->hits.clear();
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  {
    HBRUSH br = CreateSolidBrush(kFace);
    FillRect(dc, &cr, br);
    DeleteObject(br);
  }
  const double L = 32, R = SW - 32, colR = 484;
  // top: the OLAAF mark, and dmajorsound on the right
  g.fill(0, kTopH - 1, SW, kTopH, kLine);
  paintBrand(st, g, kTopH / 2);
  drawDmajorLogo(g.dc, g.X(R), g.Y(kTopH / 2), g.S(16), kMute, kMute, st->fSmall);

  // the headline, with a paper plane coming in to land
  g.text(st->fHero, kInk, "Prepare for landing", L, 86, R, 124, DT_LEFT);
  double px = L + g.width(st->fHero, "Prepare for landing") + 16;
  g.polyAA({{px + 40, 120}, {px, 98}, {px + 18, 99}}, kBlue);
  g.polyAA({{px + 40, 120}, {px + 18, 99}, {px + 17, 84}}, theme().blueLine);
  g.fill(px - 2, 123, px + 54, 125, kLine); // the runway
  bool cut = false;
  double y = 130;
  for (auto &ln : fitLines(g, st->fBody, "Bring an edit from the picture department into REAPER: its tracks, clips, fades and levels, "
                                         "at the right timecode.", R - L, 2, &cut)) {
    g.text(st->fBody, kMute, ln, L, y, R, y + 19, DT_LEFT);
    y += 19;
  }

  // three steps
  struct Step { const char *title, *text; };
  static const Step steps[3] = {
      {"Choose the file", "The AAF the editor sent you. Several reels? Choose them all at once."},
      {"Look it over", "Tracks, timecode and missing audio. Nothing changes until you import."},
      {"Land in REAPER", "OLAAF builds the tracks in the project you have open."}};
  y = 190;
  for (int i = 0; i < 3; i++) {
    g.rfill(L, y, L + 26, y + 26, 13, kBlue);
    g.text(st->fBold, kWhiteFixed, strprintf("%d", i + 1), L, y, L + 26, y + 26, DT_CENTER);
    g.text(st->fBold, kInk, steps[i].title, L + 40, y + 2, colR - 32, y + 22, DT_LEFT);
    double ty = y + 24;
    for (auto &ln : fitLines(g, st->fSmall, steps[i].text, colR - 32 - (L + 40), 2, &cut)) {
      g.text(st->fSmall, kMute, ln, L + 40, ty, colR - 32, ty + 17, DT_LEFT);
      ty += 17;
    }
    y += 62;
  }

  // the file type
  g.text(st->fSmall, kMute, "What did you get?", L, 382, colR - 32, 400, DT_LEFT);
  struct Fmt { const char *name, *from; bool ready; };
  static const Fmt fmts[2] = {{"AAF", "from Avid, Premiere, Resolve, Pro Tools, Nuendo, Logic", true},
                              {"DAWproject", "from Cubase, Bitwig, Studio One \xc2\xb7 coming soon", false}};
  for (int i = 0; i < 2; i++) {
    double ry = 404 + i * 36;
    bool on = i == 0;
    if (on) g.rbox(L - 8, ry, colR - 24, ry + 32, 8, theme().blueLine, kBlueSoft);
    double cx = L + 8, cy = ry + 16;
    if (on) {
      g.rfill(cx - 8, cy - 8, cx + 8, cy + 8, 8, kBlue);
      g.rfill(cx - 3, cy - 3, cx + 3, cy + 3, 3, kWhiteFixed);
    } else
      g.rbox(cx - 8, cy - 8, cx + 8, cy + 8, 8, kLine, kPanel);
    double nx = L + 26, nw = g.width(st->fBold, fmts[i].name);
    g.text(st->fBold, fmts[i].ready ? kInk : kMute, fmts[i].name, nx, ry, nx + nw + 4, ry + 32, DT_LEFT);
    std::vector<std::string> fl = fitLines(g, st->fSmall, fmts[i].from, colR - 32 - (nx + nw + 10), 1, &cut);
    g.text(st->fSmall, kMute, fl.empty() ? "" : fl[0], nx + nw + 10, ry, colR - 32, ry + 32, DT_LEFT);
    addHit(st, L - 8, ry, colR - 24, ry + 32, fmts[i].ready ? H_START_FMT : H_NONE, i,
           fmts[i].ready ? "AAF: the usual way to hand an edit to sound. Most picture editors can export it."
                         : "DAWproject files can't be opened yet. Ask for an AAF in the meantime.");
  }

  // the AAFs opened before
  g.fill(colR - 8, 190, colR - 7, 470, kLine);
  g.text(st->fSmall, kMute, "Opened before", colR + 12, 186, R, 204, DT_LEFT);
  const StartData *sd = st->start;
  if (!sd || sd->recent.empty()) {
    double ey = 210;
    for (auto &ln : fitLines(g, st->fSmall, "The AAFs you open show up here, so the next landing is one click away.",
                             R - colR - 12, 3, &cut)) {
      g.text(st->fSmall, kMute, ln, colR + 12, ey, R, ey + 17, DT_LEFT);
      ey += 17;
    }
  } else
    for (size_t i = 0; i < sd->recent.size() && i < 6; i++) {
      double ry = 210 + i * 44;
      const std::string &path = sd->recent[i];
      bool there = fileExists(path);
      bool hov = st->startHover == (int)i;
      if (hov && there) g.rfill(colR + 4, ry, R + 4, ry + 40, 8, kBlueSoft);
      std::vector<std::string> nl = fitLines(g, st->fBody, baseName(path), R - colR - 16, 1, &cut);
      g.text(st->fBody, there ? kInk : kMute, nl.empty() ? "" : nl[0], colR + 12, ry + 3, R, ry + 22, DT_LEFT);
      std::string where = there ? shortPath(dirName(path), 60) : std::string("Not found: ") + shortPath(dirName(path), 48);
      std::vector<std::string> wl = fitLines(g, st->fTiny, where, R - colR - 16, 1, &cut);
      g.text(st->fTiny, there ? kMute : kRed, wl.empty() ? "" : wl[0], colR + 12, ry + 22, R, ry + 38, DT_LEFT);
      if (there) addHit(st, colR + 4, ry, R + 4, ry + 40, H_START_RECENT, (int)i, "Open this AAF again.");
      else addHit(st, colR + 4, ry, R + 4, ry + 40, H_NONE, (int)i, "This AAF isn't where it was. Choose it with the button below.");
    }

  // bottom: a hint, Cancel and the one main button
  g.fill(0, SH - 60, SW, SH - 59, kLine);
  double bt = SH - 46, bb = SH - 14;
  const char *pl = "Choose AAF\xe2\x80\xa6";
  double pw = std::max(150.0, g.width(st->fBold, pl) + 40);
  button(st, g, R - pw, bt, R, bb, pl, H_START_PICK, 0, "Opens the file chooser (Return).", true);
  double cw = g.width(st->fBody, "Cancel") + 24;
  g.text(st->fBody, kMute, "Cancel", R - pw - 8 - cw, bt, R - pw - 8, bb, DT_CENTER);
  addHit(st, R - pw - 8 - cw, bt, R - pw - 8, bb, H_START_CANCEL, 0, "Close OLAAF (Esc).");
  double hintR = R - pw - 16 - cw;
  { // a newer OLAAF: right before Cancel, like in the import window's footer
    double nl = paintUpdateNote(st, g, hintR, bt, bb);
    if (nl < hintR) hintR = nl - 16;
  }
  const char *hint = st->tip ? st->tip : "Return opens the file chooser, Esc closes this window.";
  std::vector<std::string> hl = fitLines(g, st->fSmall, hint, hintR - L, 2, &cut);
  double hy = (bt + bb) / 2 - hl.size() * 8.5;
  for (auto &ln : hl) {
    g.text(st->fSmall, st->tip ? kInk : kMute, ln, L, hy, hintR, hy + 17, DT_LEFT);
    hy += 17;
  }
}

static void startPick(State *st) {
  if (!st->start || !st->start->pick) return;
  std::vector<std::string> files = st->start->pick(st->hwnd);
  if (files.empty()) return; // cancelled: the start window stays
  st->start->result = files;
  EndDialog(st->hwnd, 1);
}

INT_PTR CALLBACK startProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  State *st = (State *)OL_GETUSER(hwnd);
  if (!st && msg != WM_INITDIALOG) return 0;
  switch (msg) {
  case WM_INITDIALOG: {
    st = (State *)lp;
    OL_SETUSER(hwnd, st);
    st->hwnd = hwnd;
#ifdef _WIN32
    st->base = st->scale = windowDpi(hwnd) / 96.0;
    RECT want = {0, 0, (LONG)std::lround(SW * st->base), (LONG)std::lround(SH * st->base)};
    AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT pr;
    HWND par = GetParent(hwnd);
    if (par && GetWindowRect(par, &pr))
      SetWindowPos(hwnd, NULL, std::max((int)pr.left, (int)(pr.left + pr.right - w) / 2),
                   std::max((int)pr.top, (int)(pr.top + pr.bottom - h) / 2), w, h, SWP_NOZORDER);
#endif
    olSetText(hwnd, "OLAAF \xe2\x80\x93 AAF Import for Reaper");
    g_startHwnd = hwnd;
    relayoutStart(st);
    SetTimer(hwnd, kUpdTimer, 1000, NULL); // is there a newer OLAAF?
    SetFocus(hwnd);
    return 0;
  }
  case WM_TIMER:
    if (wp == kUpdTimer) {
      std::string tag, url;
      int u = updateCheckState(&tag, &url);
      if (u == 1) {
        st->updTag = tag;
        st->updUrl = url;
        SetTimer(hwnd, kBlinkTimer, 700, NULL);
        InvalidateRect(hwnd, NULL, FALSE);
      }
      if (u != 0 || ++st->updPollsStart > 20) KillTimer(hwnd, kUpdTimer);
    } else if (wp == kBlinkTimer) {
      st->blinkOn = !st->blinkOn;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  case WM_ERASEBKGND: return 1;
  case WM_SIZE:
    relayoutStart(st);
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  case WM_GETMINMAXINFO: {
    MINMAXINFO *mm = (MINMAXINFO *)lp;
    mm->ptMinTrackSize.x = (LONG)(SW * st->base * 0.7);
    mm->ptMinTrackSize.y = (LONG)(SH * st->base * 0.7);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
#ifdef _WIN32
    RECT cr;
    GetClientRect(hwnd, &cr);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    paintStart(st, mem);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
#else
    paintStart(st, dc);
#endif
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_MOUSEMOVE: {
    const Hit *h = hitAt(st, (short)LOWORD(lp), (short)HIWORD(lp));
    const char *tip = h ? h->tip : nullptr;
    int hov = h && h->id == H_START_RECENT ? h->idx : -1;
    if (tip != st->tip || hov != st->startHover) {
      st->tip = tip;
      st->startHover = hov;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  }
  case WM_LBUTTONUP: {
    const Hit *h = hitAt(st, (short)LOWORD(lp), (short)HIWORD(lp));
    if (!h) return 0;
    if (h->id == H_START_PICK) startPick(st);
    else if (h->id == H_UPDATE && !st->updUrl.empty()) openWebPage(st->updUrl);
    else if (h->id == H_START_CANCEL) EndDialog(hwnd, 0);
    else if (h->id == H_START_RECENT && st->start && h->idx >= 0 && h->idx < (int)st->start->recent.size()) {
      st->start->result = {st->start->recent[(size_t)h->idx]};
      EndDialog(hwnd, 1);
    }
    return 0;
  }
#ifdef _WIN32
  case WM_GETDLGCODE:
    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DLGC_WANTALLKEYS);
    return TRUE;
#endif
  case WM_KEYDOWN:
    if (wp == VK_RETURN) {
      startPick(st);
      return 1;
    }
    if (wp == VK_ESCAPE) {
      EndDialog(hwnd, 0);
      return 1;
    }
    return 0;
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK) startPick(st);
    else if (LOWORD(wp) == IDCANCEL) EndDialog(hwnd, 0);
    return 0;
  case WM_CLOSE:
    EndDialog(hwnd, 0);
    return 0;
  case WM_DESTROY:
    KillTimer(hwnd, kUpdTimer);
    KillTimer(hwnd, kBlinkTimer);
    if (g_startHwnd == hwnd) g_startHwnd = nullptr;
    freeFonts(st);
    return 0;
  }
  return 0;
}

std::vector<std::string> showStartWindow(HWND parent, const std::vector<std::string> &recent,
                                         std::function<std::vector<std::string>(HWND parent)> pick) {
  StartData sd;
  sd.recent = recent;
  sd.pick = std::move(pick);
  State st;
  st.start = &sd;
#ifdef _WIN32
  DlgTemplate dt;
  memset(&dt, 0, sizeof(dt));
  dt.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
  dt.t.cx = 380;
  dt.t.cy = 270;
  wcscpy(dt.title, L"OLAAF");
  DialogBoxIndirectParam((HINSTANCE)g_hInst, &dt.t, parent, (DLGPROC)startProc, (LPARAM)&st);
#else
  DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_OLAAF_START), parent, startProc, (LPARAM)&st);
#endif
  return sd.result;
}

HWND openDialogHwnd() { return g_open ? g_open->hwnd : nullptr; }

HWND openImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host,
                      std::function<void(bool accepted)> onClose) {
  if (g_open) { // one at a time
    ShowWindow(g_open->hwnd, SW_SHOW);
    SetForegroundWindow(g_open->hwnd);
    return nullptr;
  }
  State *st = new State;
  st->imp = &imp;
  st->opt = &opt;
  st->host = &host;
  st->merge = opt.mergeStereo;
  st->modeless = true;
  st->onClose = std::move(onClose);
  HWND h;
#ifdef _WIN32
  static DlgTemplate dt;
  memset(&dt, 0, sizeof(dt));
  dt.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
  dt.t.cx = 600;
  dt.t.cy = 400;
  wcscpy(dt.title, L"OLAAF");
  h = CreateDialogIndirectParam((HINSTANCE)g_hInst, &dt.t, parent, (DLGPROC)dlgProc, (LPARAM)st);
#else
  h = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_OLAAF_IMPORT), parent, dlgProc, (LPARAM)st);
#endif
  if (!h) {
    delete st;
    return nullptr;
  }
  ShowWindow(h, SW_SHOW);
  return h;
}

bool showImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host) {
  State st;
  st.imp = &imp;
  st.opt = &opt;
  st.host = &host;
  st.merge = opt.mergeStereo;
#ifdef _WIN32
  DlgTemplate dt;
  memset(&dt, 0, sizeof(dt));
  dt.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;
  dt.t.cx = 600;
  dt.t.cy = 400;
  wcscpy(dt.title, L"OLAAF");
  DialogBoxIndirectParam((HINSTANCE)g_hInst, &dt.t, parent, (DLGPROC)dlgProc, (LPARAM)&st);
#else
  DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_OLAAF_IMPORT), parent, dlgProc, (LPARAM)&st);
#endif
  return st.accepted;
}

} // namespace olaaf
