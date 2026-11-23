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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "dialog.h"
#include "theme.h"
#include "logo.h"
#include "../core/util.h"
#include "../core/audiofile.h"
#include "../core/peaks.h"
#include "fonts.h"


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

const int W = 1180, H = 800;

#define kFace (theme().face)
#define kPanel (theme().panel)
#define kLine (theme().line)
#define kSoft (theme().soft)
#define kMute (theme().mute)
#define kInk (theme().ink)
#define kBlue (theme().blue)
#define kBlueSoft (theme().blueSoft)
#define kRed (theme().red)
const int kPeakTimer = 7;
const COLORREF kWhiteFixed = RGB(0xff, 0xff, 0xff);
// text on the blue accent: white in light mode, near-black on the brighter dark-mode blue
#define kWhite (olaaf::darkTheme() ? RGB(0x12, 0x1a, 0x26) : kWhiteFixed)
#define kCounterBg kPanel
#define kCounterInk kBlue

const uint32_t kPalette[10] = {0x5580d4, 0x1F9A8A, 0x4E8F3A, 0xB8962E, 0xD9822B,
                               0xC8423B, 0xB0457E, 0x7A5AC8, 0x6B7480, 0x2B3440};

COLORREF rgbToRef(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// ------------------------------------------------------------------ geometry (layout pixels)
// header 0-56, toolbar 56-108, arrange window 108-633, event bar, media bar, footer
const double kL = 16, kR = W - 16;
const double kHeadH = 56;
const double kAT = 108;                       // arrange window top
const double kHdr = 236;                      // lane header width
const double kRuler = 30, kRowH = 36, kSb = 12;
const int kRows = 13;
const double kLT = kAT + 1 + kRuler;          // first lane
const double kLB = kLT + kRows * kRowH;       // below the last lane
const double kAB = kLB + kSb + 2;             // arrange window bottom
const double kTlx = kL + kHdr + 1;            // timeline left
const double kTlr = kR - 1 - kSb;             // timeline right (vertical scrollbar beside it)
const double kEvT = kAB + 10, kEvB = kEvT + 44;   // event bar
const double kMdT = kEvB + 8, kMdB = kMdT + 52;   // media bar
const double kFootY = kMdB + 10;                  // footer divider

enum HitId {
  H_NONE, H_THEME, H_VIDEO, H_MARKERS, H_FOLDERS, H_ZOOM_FIT, H_TRACK_CHECK, H_TRACK_COLOR,
  H_ZOOM_IN, H_ZOOM_OUT, H_MERGE, H_FADES, H_GAIN, H_COPY_YES, H_COPY_NO, H_SUBDIR, H_DEST_CHOOSE, H_LOCATE,
  H_CANCEL, H_IMPORT, H_TC_PLUS, H_TC_MINUS, H_TC_VALUE, H_TC_CURSOR, H_PAL, H_PAL_CUSTOM, H_PAL_NONE,
  H_LANES, H_TRACK_NAME, H_FOLD_ASSIGN, H_FOLD_NONE, H_FOLD_RENAME, H_FOLD_DELETE, H_FOLD_NEW, H_FOLD_POP,
  H_FOLD_COLOR, H_PAL_BG, H_FROW_RENAME, H_FROW_COLOR, H_EV_REMOVE, H_EV_RESTORE, H_EV_UP, H_EV_DOWN,
  H_EV_AUTOMATION, H_EV_RESET, H_EV_MUTE, H_NEW_FOLDER, H_TRACK_FOLDERMENU, H_TRACK_DELETE, H_VIDEO_CHOOSE, H_SHOW_AUTO, H_TRACK_DEST, H_DEST_NEW, H_DEST_MATCHALL, H_DEST_PICK, H_DEST_POP, H_DEST_OVERWRITE
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
        fMonoSmall = nullptr;
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
}

void freeFonts(State *st) {
  HFONT *fs[] = {&st->fTheme, &st->fCheck, &st->fClip, &st->fTitle, &st->fTitleThin, &st->fTag, &st->fBody,
                 &st->fBold, &st->fSmall, &st->fSec, &st->fMono, &st->fMonoBig, &st->fMonoSmall};
  for (HFONT *f : fs) {
    if (*f) DeleteObject(*f);
    *f = nullptr;
  }
}

// The layout is drawn at a fixed size (W x H) and zoomed to the window: everything, type included,
// grows with the window. The zoomed layout is centered in the window.
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
  void check(double x, double y, COLORREF c) const {
    line(x + 3.75, y + 8.25, x + 6.75, y + 11.25, c, 2);
    line(x + 6.75, y + 11.25, x + 12.25, y + 4.75, c, 2);
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
    g.box(x, y, x + 16, y + 16, kBlue, kBlue);
    g.check(x, y, kWhite);
  } else
    g.box(x, y, x + 16, y + 16, theme().empty, kPanel);
  addHit(st, x - 4, y - 4, x + 20, y + 20, id, idx, tip);
}

void button(State *st, const Gfx &g, double l, double t, double r, double b, const std::string &label, int id,
            int idx, const char *tip, bool primary = false, bool enabled = true) {
  if (primary) {
    g.fill(l, t, r, b, enabled ? kBlue : kLine);
    g.text(st->fBold, kWhite, label, l, t, r, b, DT_CENTER);
  } else {
    g.box(l, t, r, b, kLine, kPanel);
    if (!g.icon(label, (l + r) / 2, (t + b) / 2, enabled ? kInk : kMute, kPanel))
      g.text(st->fBody, enabled ? kInk : kMute, label, l, t, r, b, DT_CENTER);
  }
  if (enabled) addHit(st, l, t, r, b, id, idx, tip);
}

// a switch in the toolbar: a plain checkbox with its label; returns its right edge
double chip(State *st, const Gfx &g, double x, double t, const std::string &label, bool on, bool enabled, int id,
            const char *tip) {
  double tw = g.width(st->fBody, label);
  if (tw < 4 || tw > 220) tw = 7 * label.size();
  double w = 16 + 7 + tw + 14;
  double cy = t + 13 - 8;
  if (!enabled) g.box(x, cy, x + 16, cy + 16, kLine, kFace);
  else if (on) {
    g.fill(x, cy, x + 16, cy + 16, kBlue);
    g.check(x, cy, kWhite);
  } else
    g.box(x, cy, x + 16, cy + 16, theme().empty, kPanel);
  g.text(st->fBody, enabled ? kInk : theme().empty, label, x + 23, t, x + w, t + 26, DT_LEFT);
  addHit(st, x - 2, t, x + w - 8, t + 26, enabled ? id : H_NONE, 0, tip);
  return x + w;
}

double niceStep(double secPerPx) {
  static const double steps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
  for (double s : steps)
    if (s / secPerPx >= 110) return s;
  return 7200;
}

double tcOffset(const State *st) { return st->tcMinus ? -st->tcAbs : st->tcAbs; }

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
    for (auto &f : s.folders) {
      Row fr;
      fr.kind = 1;
      fr.folder = f.key;
      st->rows.push_back(fr);
      for (size_t i = 0; i < s.tracks.size(); i++)
        if (s.tracks[i].group == f.key) {
          st->rows.push_back({0, (int)i, std::string()});
          st->order.push_back((int)i);
          placed[i] = true;
        }
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

// shown track position at row index ri (a folder row counts as the track below it; past the end = new tracks)
int posAtRow(const State *st, int ri) {
  int pos = 0;
  for (int i = 0; i < (int)st->rows.size() && i < ri; i++)
    if (st->rows[(size_t)i].kind == 0) pos++;
  if (ri >= (int)st->rows.size()) pos += ri - (int)st->rows.size() + 1;
  return pos;
}

// row index that shows shown track position pos (new tracks go on and below the drop line)
int rowOfPos(const State *st, int pos) {
  int p = 0;
  for (size_t i = 0; i < st->rows.size(); i++)
    if (st->rows[i].kind == 0) {
      if (p == pos) return (int)i;
      p++;
    }
  return (int)st->rows.size() - 1 + (pos - (int)st->order.size());
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

void paintHeader(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  g.fill(0, 0, W, kHeadH, kPanel);
  g.fill(0, kHeadH - 1, W, kHeadH, kLine);
  g.fill(16, 13, 46, 43, kBlue);
  g.fill(23, 21, 33, 24.5, kWhiteFixed);
  g.fill(27, 26.4, 39, 29.9, kWhiteFixed);
  g.fill(23, 31.8, 30, 35.3, kWhiteFixed);
  g.text(st->fTitle, kInk, "OLAAF", 56, 6, 300, 32, DT_LEFT);
  double tw = g.width(st->fTitle, "OLAAF");
  if (tw < 20 || tw > 200) tw = 55;
  g.text(st->fTitleThin, kMute, "\xe2\x80\x93 AAF Import for Reaper", 56 + tw + 5, 6, 360, 32, DT_LEFT);
  g.text(st->fTag, kMute, "Do you wanna build a Reaper Session?", 56, 30, 360, 45, DT_LEFT);

  double x = 352;
  g.fill(x, 13, x + 1, 43, kLine);
  x += 14;
  int events = 0;
  for (auto &t : s.tracks) events += (int)t.clips.size();
  std::string name = baseName(s.aafPath);
  std::string meta = strprintf("%s \xc2\xb7 %g kHz \xc2\xb7 %d bit \xc2\xb7 %zu tracks \xc2\xb7 %d events", s.product.c_str(),
                               s.sampleRate / 1000.0, s.bits, s.tracks.size(), events);
  double nw = std::max(g.width(st->fBold, name), g.width(st->fSmall, meta));
  nw = std::min(std::max(nw, 120.0), 330.0);
  g.text(st->fBold, kInk, name, x, 11, x + nw, 29, DT_LEFT);
  g.text(st->fSmall, kMute, meta, x, 28, x + nw, 44, DT_LEFT);
  x += nw + 16;
  struct Stat { const char *k; std::string v; } stats[2] = {{"Frame rate", fpsText(s)},
                                                           {"AAF starts at", formatTC(s.firstClip(), s.fps)}};
  for (auto &stt : stats) {
    double w = std::max(g.width(st->fMono, stt.v), g.width(st->fSec, stt.k)) + 26;
    if (w < 60 || w > 220) w = 130;
    g.fill(x, 13, x + 1, 43, kLine);
    g.text(st->fSec, kMute, stt.k, x + 12, 11, x + w, 25, DT_LEFT);
    g.text(st->fMono, kInk, stt.v, x + 12, 24, x + w, 44, DT_LEFT);
    x += w;
  }
  addHit(st, 366, 8, x, 48, H_NONE, 0, "The AAF as it comes in: frame rate and where its timeline starts.");
  drawDmajorLogo(g.dc, g.X(W - 16), g.Y(28), g.S(30), kInk, kMute, st->fSmall);
}

void paintToolbar(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double t = 69, mid = 82;
  double x = kL;
  std::string mLabel = s.markers.empty() ? "Markers" : strprintf("Markers %zu", s.markers.size());
  bool videoOk = !s.video.empty();
  x = chip(st, g, x, t, "Combine .L / .R", st->merge, true, H_MERGE,
           "Combines two mono tracks named .L and .R whose clips line up into one stereo track.") + 6;
  x = chip(st, g, x, t, "Fades", opt.fades, true, H_FADES, "Fade ins, fade outs and crossfades from the AAF.") + 6;
  x = chip(st, g, x, t, !opt.clipGain ? "Clip gain" : (opt.clipGainToTrack ? "Clip gain \xe2\x86\x92 track" : "Clip gain"),
           opt.clipGain, true, H_GAIN,
           "Click to switch: clip gain on the items (and clip automation as take envelopes) \xe2\x86\x92 clip gain folded into "
           "the track volume envelope \xe2\x86\x92 off.") + 6;
  if (videoOk) {
    bool manual = s.video[0].manual;
    x = chip(st, g, x, t, !fileExists(s.video[0].foundPath) ? "Video (missing)" : "Video", opt.video, true, H_VIDEO,
             manual ? "The picture you chose, on a VIDEO track at the top, at its own timecode (or where the AAF starts)."
                    : "The picture the AAF points to, on a VIDEO track at the top.") + 2;
    if (manual) // choose another file
      x = chip(st, g, x, t, "\xe2\x80\xa6", false, true, H_VIDEO_CHOOSE, "Choose another video file.") + 6;
    else
      x += 4;
  } else
    x = chip(st, g, x, t, "Video\xe2\x80\xa6", false, true, H_VIDEO_CHOOSE,
             "This AAF has no video. Choose the picture: it goes on a VIDEO track at the top, at its own timecode (or where the AAF starts).") + 6;
  x = chip(st, g, x, t, mLabel, opt.markers && !s.markers.empty(), !s.markers.empty(), H_MARKERS,
           s.markers.empty() ? "This AAF has no markers." : "Markers from the edit become REAPER markers.") + 6;
  chip(st, g, x, t, "Folders", opt.folders, true, H_FOLDERS,
       "Folder tracks. Click a track name to choose its folder; the pencil renames a folder.");

  // timecode offset: + / -, counter, where it lands in REAPER, edit cursor
  double r = kR;
  double ecW = 96;
  button(st, g, r - ecW, mid - 17, r, mid + 17, "Edit cursor", H_TC_CURSOR, 0,
         "Sets the offset so the AAF lands at the REAPER edit cursor.");
  r -= ecW + 8;
  double landW = 96;
  g.text(st->fSmall, kMute, "in REAPER at", r - landW, mid - 16, r, mid, DT_LEFT);
  g.text(st->fMonoSmall, kInk, formatTC(s.firstClip() + tcOffset(st), s.fps), r - landW, mid, r, mid + 16, DT_LEFT);
  r -= landW + 8;
  double cw = 176;
  if (st->editKind != 1) g.text(st->fMonoBig, kBlue, formatTC(st->tcAbs, s.fps), r - cw, mid - 17, r, mid + 15, DT_CENTER);
  for (double dx = r - cw + 8; dx < r - 8; dx += 6) g.fill(dx, mid + 15, dx + 3, mid + 16, theme().empty); // click to type
  st->tcRc = devRect(st, r - cw + 10, mid - 11, r - 10, mid + 11);
  addHit(st, r - cw, mid - 17, r, mid + 17, H_TC_VALUE, 0,
         "Type the offset: HH:MM:SS:FF or just the digits (1000 = 10 seconds). Return applies, Esc cancels.");
  r -= cw + 6;
  // + / -
  double sb = r - 52;
  g.box(sb, mid - 16, sb + 26, mid + 16, kLine, st->tcMinus ? kPanel : kBlue);
  g.text(st->fBold, st->tcMinus ? kInk : kWhite, "+", sb, mid - 16, sb + 26, mid + 16, DT_CENTER);
  g.box(sb + 26, mid - 16, sb + 52, mid + 16, kLine, st->tcMinus ? kBlue : kPanel);
  g.text(st->fBold, st->tcMinus ? kWhite : kInk, "\xe2\x88\x92", sb + 26, mid - 16, sb + 52, mid + 16, DT_CENTER);
  addHit(st, sb, mid - 16, sb + 26, mid + 16, H_TC_PLUS, 0, "Add the offset to the AAF's timecode.");
  addHit(st, sb + 26, mid - 16, sb + 52, mid + 16, H_TC_MINUS, 0, "Subtract the offset from the AAF's timecode.");
  g.text(st->fSec, kMute, "Offset", sb - 56, mid - 13, sb - 6, mid + 13, DT_RIGHT);
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

void paintAutomation(State *st, const Gfx &g, const Track &tr, double y, double spp) {
  const ImportOptions &opt = *st->opt;
  double top = y + 4, bot = y + kRowH - 5;
  auto volY = [&](double gain) {
    double db = gain > 1e-6 ? 20.0 * std::log10(gain) : -60.0;
    double n = std::max(0.0, std::min(1.0, (db + 60.0) / 72.0));
    return bot - n * (bot - top);
  };
  const COLORREF panC = RGB(0xD9, 0x82, 0x2B);
  bool hasPan = !tr.panEnv.empty() || std::fabs(tr.pan) > 1e-4;
  double px = -1, pv = 0, pp = 0;
  for (double x = kTlx + 1; x <= kTlr - 1; x += 2) {
    double t = st->viewStart + (x - kTlx) * spp;
    double v = volY(st->imp->trackVolumeAt(tr, t, opt));
    double pn = (top + bot) / 2 - lerpEnv(tr.panEnv, t, tr.pan) * (bot - top) / 2;
    if (px >= 0) {
      g.line(px, pv, x, v, kBlue);
      if (hasPan) g.line(px, pp, x, pn, panC);
    }
    px = x;
    pv = v;
    pp = pn;
  }
  // clip automation that stays on the items: a thin line inside each clip
  if (opt.clipGain && !opt.clipGainToTrack)
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
      double a = std::sqrt(std::min(1.0, pk * gain)); // square root: quiet passages stay visible
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
  g.box(kL, kAT, kR, kAB, kLine, kPanel);

  // header corner
  double ry = kAT + 1;
  int nsel = 0;
  for (auto &t : s.tracks) nsel += t.import && !t.created;
  int real = 0;
  for (auto &t : s.tracks) real += !t.created;
  g.fill(kL + kHdr, ry, kL + kHdr + 1, kAB - 1, kLine);
  g.text(st->fSmall, kMute, strprintf("%d / %d", nsel, real), kL + 8, ry, kL + 60, ry + kRuler, DT_LEFT);
  addHit(st, kL + 4, ry, kL + 60, ry + kRuler, H_NONE, 0, "Tracks that will be imported / tracks in the AAF.");
  button(st, g, kL + 62, ry + 5, kL + 136, ry + 25, "+ Folder", H_NEW_FOLDER, 0,
         "Add a folder (with the selected tracks in it). Drag track names onto a folder to put them in.");
  double bx = kL + kHdr - 8;
  button(st, g, bx - 22, ry + 5, bx, ry + 25, "+", H_ZOOM_IN, 0, "Zoom in (+ key, or Alt / Option + mouse wheel). Shift + mouse wheel moves through time.");
  button(st, g, bx - 46, ry + 5, bx - 24, ry + 25, "\xe2\x88\x92", H_ZOOM_OUT, 0, "Zoom out (- key, or Alt / Option + mouse wheel).");
  button(st, g, bx - 82, ry + 5, bx - 48, ry + 25, "Fit", H_ZOOM_FIT, 0, "Show the whole AAF.");

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

  // view switch: automation lines in the lanes (only what you see here, not the import)
  {
    const char *lab = "Automation";
    double w = g.width(st->fSmall, lab) + 26;
    if (w < 60 || w > 140) w = 92;
    double ax = kTlr - w - 4, ay = ry + 3;
    g.box(ax, ay, ax + w, ay + 13 + 0, opt.showAutomation ? kBlue : kLine, opt.showAutomation ? kBlueSoft : kPanel);
    g.text(st->fClip, opt.showAutomation ? kBlue : kMute, lab, ax, ay, ax + w, ay + 13, DT_CENTER);
    addHit(st, ax, ay, ax + w, ay + 13, H_SHOW_AUTO, 0,
           "Show the volume (blue) and pan (orange) automation in the lanes. A view only, it changes nothing in the import.");
  }

  // lanes
  int nrows = (int)st->rows.size();
  st->scrollRow = std::max(0, std::min(st->scrollRow, std::max(0, nrows - kRows)));
  st->lanesRc = devRect(st, kTlx, kLT, kTlr, kLB);
  st->clipHits.clear();
  addHit(st, kTlx, kLT, kTlr, kLB, H_LANES, 0,
         "Click selects, Shift-click adds. Drag or arrow up / down moves events to another track, M mutes, Delete removes.");
  bool dragging = st->drag == 3 && st->dragMoved && st->dragRows != 0;
  for (int vr = 0; vr < kRows; vr++) {
    int ri = st->scrollRow + vr;
    double y = kLT + vr * kRowH;
    if (ri >= nrows) continue;
    const Row &row = st->rows[(size_t)ri];
    if (row.kind == 1) { // folder
      int fi = folderIndex(s, row.folder);
      if (fi < 0) continue;
      Folder &f = s.folders[(size_t)fi];
      int n = 0;
      for (auto &t : s.tracks) n += t.group == f.key;
      g.fill(kL + 1, y, kR - 1, y + kRowH, theme().track);
      g.fill(kL + 1, y + kRowH - 1, kR - 1, y + kRowH, kLine);
      if (st->drag == 4 && st->dragMoved && dropKey(st, st->dropRow) == f.key && st->dropRow <= (int)st->rows.size() - 1)
        g.fill(kL + 1, y, kL + kHdr, y + kRowH - 1, kBlueSoft); // the folder they go into
      g.fill(kL + 1, y, kL + 7, y + kRowH - 1, f.color ? rgbToRef(f.color) : kMute);
      addHit(st, kL + 1, y, kL + 14, y + kRowH, H_FROW_COLOR, fi, "Set the folder color.");
      g.icon("\xe2\x96\xbe", kL + 19, y + kRowH / 2.0, kMute, kSoft);
      bool renaming = st->editKind == 2 && st->editKey == f.key;
      double nw = std::min(120.0, g.width(st->fBold, f.name) + 12);
      if (!renaming) g.text(st->fBold, kInk, f.name, kL + 28, y, kL + 28 + nw, y + kRowH, DT_LEFT);
      g.text(st->fSmall, kMute, strprintf("folder \xc2\xb7 %d track%s", n, n == 1 ? "" : "s"), kL + 34 + nw, y,
             kL + kHdr - 30, y + kRowH, DT_LEFT);
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
    double ind = inF ? 16 : 0, hx = kL + ind;
    if (!on) g.fill(kL + 1, y, kR - 1, y + kRowH - 1, theme().track);
    if (inF) g.fill(kL + 1, y, kL + 4, y + kRowH, inF->color ? rgbToRef(inF->color) : kMute);
    bool trSel = st->selTracks.count(row.track) > 0;
    bool grabbed = trSel && st->drag == 4 && st->dragMoved;
    if (trSel) g.fill(hx + 7, y, kL + kHdr, y + kRowH - 1, kBlueSoft);
    g.fill(hx + 1, y, hx + 7, y + kRowH - 1, on ? tc : theme().off);
    addHit(st, hx + 1, y, hx + 14, y + kRowH, H_TRACK_COLOR, row.track,
           "Set the track color. Without one, a track takes the color of its folder.");
    double cy = y + (kRowH - 18) / 2;
    if (on) {
      g.fill(hx + 20, cy, hx + 38, cy + 18, kBlue);
      g.fill(hx + 26, cy + 6, hx + 32, cy + 12, kWhite);
    } else
      g.box(hx + 20, cy, hx + 38, cy + 18, theme().empty, kPanel);
    addHit(st, hx + 16, y + 2, hx + 42, y + kRowH - 2, H_TRACK_CHECK, row.track, "Import this track or leave it out.");
    double nameR = kL + kHdr - 34;
    if (st->host && st->host->projectTracks) { // put the events on an existing REAPER track
      bool matched = !tr.target.empty();
      g.icon("\xe2\x87\xa5", nameR - 9, y + kRowH / 2.0, matched ? kBlue : kMute, kPanel);
      addHit(st, nameR - 18, y + 4, nameR, y + kRowH - 4, H_TRACK_DEST, row.track,
             "Put this track on a track that is already in your REAPER project (or on a new one).");
      nameR -= 18;
    }
    if (opt.folders) {
      g.icon("\xe2\x96\xbe", nameR - 9, y + kRowH / 2.0, kMute, kPanel);
      addHit(st, nameR - 18, y + 4, nameR, y + kRowH - 4, H_TRACK_FOLDERMENU, row.track, "Choose the folder for this track.");
      nameR -= 18;
    }
    if (tr.channels >= 2) {
      const char *badge = tr.channels == 2 ? (tr.mergedFrom.empty() ? "ST" : "L+R") : "MC";
      g.box(nameR - 30, y + 11, nameR - 2, y + 25, theme().blueLine, kBlueSoft);
      g.text(st->fClip, kBlue, badge, nameR - 30, y + 11, nameR - 2, y + 25, DT_CENTER);
      nameR -= 34;
    }
    COLORREF nameC = (on && !grabbed) ? kInk : kMute;
    if (st->editKind == 4 && st->editTrack == row.track) nameC = trSel ? kBlueSoft : kPanel; // the editor sits on top
    double nx = hx + 48;
    if (!tr.target.empty()) {
      g.text(st->fBody, nameC, tr.name, nx, y + 3, nameR, y + 20, DT_LEFT);
      g.text(st->fClip, kBlue, "\xe2\x86\x92 " + tr.targetName + (opt.overwriteEnvelopes ? "  (envelopes overwritten)" : ""), nx,
             y + 19, nameR, y + kRowH - 3, DT_LEFT);
    } else if (!tr.mergedFrom.empty()) {
      g.text(st->fBody, nameC, tr.name, nx, y + 3, nameR, y + 20, DT_LEFT);
      g.text(st->fClip, kMute, tr.mergedFrom, nx, y + 19, nameR, y + kRowH - 3, DT_LEFT);
    } else
      g.text(st->fBody, nameC, tr.name, nx, y, nameR, y + kRowH, DT_LEFT);
    if (!on) g.fill(nx, y + kRowH / 2, std::min(nameR, nx + g.width(st->fBody, tr.name)), y + kRowH / 2 + 1, kMute);
    if (grabbed) g.fill(kL + kHdr - 3, y, kL + kHdr, y + kRowH - 1, kBlue); // picked up
    addHit(st, hx + 44, y + 2, nameR, y + kRowH - 2, H_TRACK_NAME, row.track,
           "Click selects, Shift or Cmd / Ctrl-click adds, double-click renames. Drag over names to select several, drag selected names to move them.");
    int active = 0;
    for (auto &c : tr.clips) active += !c.excluded;
    if (tr.created) { // a track added here can be deleted again; AAF tracks cannot
      button(st, g, kL + kHdr - 26, y + 7, kL + kHdr - 8, y + kRowH - 7, "\xc3\x97", H_TRACK_DELETE, row.track,
             "Delete this new track. Its events go back to the AAF tracks they came from.");
    } else
      g.text(st->fSmall, kMute, std::to_string(active), kL + kHdr - 34, y, kL + kHdr - 8, y + kRowH, DT_RIGHT);

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
        int tr2 = rowOfPos(st, st->dragFromPos + st->dragRows + (posOfTrack(st, row.track) - st->dragFromPos));
        int gv = tr2 - st->scrollRow;
        if (gv >= 0 && gv < kRows) {
          double gy = kLT + gv * kRowH;
          g.box(x0, gy + 4, x1, gy + kRowH - 4, kBlue, kBlueSoft);
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
        if (selected) g.fill(x0 - 2, y0 - 2, x1 + 2, y1 + 2, kInk);
        if (selected) g.fill(x0 - 1, y0 - 1, x1 + 1, y1 + 1, kPanel);
        g.fill(x0, y0, x1, y1, tint);
        paintWave(st, g, c, x0 + 2, wy, x1, y1, cx0, spp, wave);
        g.fill(x0, y0, std::min(x1, x0 + 2), y1, cc); // the clip start
        double fi = c.fadeIn.len / spp, fo = c.fadeOut.len / spp;
        if (opt.fades && fi > 2) g.line(x0, y1, std::min(x1, x0 + fi), y0, kInk);
        if (opt.fades && fo > 2) g.line(std::max(x0, x1 - fo), y0, x1, y1, kInk);
      }
      if (x1 - x0 > 30 && !c.name.empty())
        g.text(st->fClip, (c.excluded || c.mute || !on) ? kMute : kInk, c.mute ? "M  " + c.name : c.name, x0 + 4, y0,
               x1 - 3, y0 + 12, DT_LEFT);
      ClipHit h;
      h.r = devRect(st, x0, y0, x1, y1);
      h.id = c.id;
      st->clipHits.push_back(h);
    }
    if (opt.showAutomation && on) paintAutomation(st, g, tr, y, spp);
  }

  // moving tracks: a line where they will land, and what is being carried
  if (st->drag == 4 && st->dragMoved && st->dropRow >= 0) {
    int slot = std::min(st->dropRow, nrows - 1);
    double ly = kLT + (slot - st->scrollRow) * kRowH;
    if (ly >= kLT - 1 && ly <= kLB + 1) {
      std::string key = dropKey(st, st->dropRow);
      double ix = kL + ((opt.folders && !key.empty()) ? 16 : 0);
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
}

void paintEventBar(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  double t = kEvT, b = kEvB, mid = (t + b) / 2;
  g.box(kL, t, kR, b, kLine, kPanel);
  g.text(st->fSec, kMute, "Event", kL + 12, t, kL + 60, b, DT_LEFT);
  std::vector<int> ids(st->sel.begin(), st->sel.end());
  double right = kR - 10;
  if (!ids.empty()) {
    bool anyIn = false, allMuted = true;
    for (int id : ids)
      if (Clip *k = st->imp->findClip(id)) {
        anyIn = anyIn || !k->excluded;
        allMuted = allMuted && k->mute;
      }
    // right to left: automation, down, up, remove, mute
    double aw = 128;
    checkbox(st, g, right - aw, mid - 8, st->moveAutomation, H_EV_AUTOMATION, 0,
             "Volume and pan automation under a moved event goes with it to the new track.");
    g.text(st->fBody, kInk, "Automation along", right - aw + 22, t, right, b, DT_LEFT);
    right -= aw + 10;
    button(st, g, right - 30, mid - 14, right, mid + 14, "\xe2\x96\xbc", H_EV_DOWN, 0,
           "One track down (arrow down). Below the last track a new track is added.");
    button(st, g, right - 62, mid - 14, right - 32, mid + 14, "\xe2\x96\xb2", H_EV_UP, 0, "One track up (arrow up).");
    right -= 70;
    button(st, g, right - 84, mid - 14, right, mid + 14, anyIn ? "Remove" : "Restore", anyIn ? H_EV_REMOVE : H_EV_RESTORE, 0,
           anyIn ? "Leaves the selected events out (Delete). They stay visible and can be restored." :
                   "Puts the selected events back into the import (Delete).");
    right -= 92;
    button(st, g, right - 70, mid - 14, right, mid + 14, allMuted ? "Unmute" : "Mute", H_EV_MUTE, 0,
           "Imports the selected events as muted items (M).");
    right -= 78;
  }
  if (st->edited) {
    button(st, g, right - 90, mid - 14, right, mid + 14, "Reset edits", H_EV_RESET, 0,
           "Undo every remove, mute and move made here.");
    right -= 100;
  }
  double x = kL + 64;
  if (ids.empty()) {
    g.text(st->fBody, kMute, "Click an event to select it. Shift-click selects more.", x, t, right, b, DT_LEFT);
    return;
  }
  int ti = -1;
  Clip *c = st->imp->findClip(ids[0], &ti);
  if (c && ids.size() == 1) {
    std::string nm = c->name.empty() ? "(no name)" : c->name;
    double nw = std::min(240.0, g.width(st->fBold, nm) + 10);
    g.text(st->fBold, kInk, nm, x, t, x + nw, b, DT_LEFT);
    std::string det = strprintf("%s \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s%s \xc2\xb7 fades %.2f / %.2f s%s",
                                ti >= 0 ? s.tracks[(size_t)ti].name.c_str() : "-",
                                formatTC(c->pos + tcOffset(st), s.fps).c_str(), formatTC(c->len, s.fps).c_str(),
                                dbText(c->gain).c_str(), c->gainEnv.empty() ? "" : " + curve", c->fadeIn.len,
                                c->fadeOut.len, c->excluded ? " \xc2\xb7 removed" : (c->mute ? " \xc2\xb7 muted" : ""));
    g.text(st->fSmall, kMute, det, x + nw + 6, t, right - 6, b, DT_LEFT);
  } else {
    std::set<int> trs;
    for (int id : ids) {
      int k = -1;
      if (st->imp->findClip(id, &k)) trs.insert(k);
    }
    g.text(st->fBold, kInk, strprintf("%zu events on %zu track%s", ids.size(), trs.size(), trs.size() == 1 ? "" : "s"), x,
           t, right - 6, b, DT_LEFT);
  }
}

void paintMediaBar(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double t = kMdT, b = kMdB, mid = (t + b) / 2;
  g.text(st->fSec, kMute, "Media", kL, t, kL + 50, b, DT_LEFT);
  int total = 0, ok = 0;
  std::set<int> used;
  for (auto &tr : s.tracks)
    if (tr.import)
      for (auto &c : tr.clips)
        if (!c.excluded)
          for (auto &src : c.sources)
            if (src.media >= 0) used.insert(src.media);
  for (int mi : used) {
    total++;
    const Media &m = s.media[(size_t)mi];
    if (m.kind == MediaKind::Embedded || fileExists(m.foundPath)) ok++;
  }
  double x = kL + 52;
  if (ok == total) {
    std::string txt = strprintf("%d / %d found", ok, total);
    double w = g.width(st->fBody, txt) + 8;
    g.text(st->fBody, theme().green, txt, x, t, x + w, b, DT_LEFT);
    addHit(st, x, t + 8, x + w, b - 8, H_NONE, 0, "Every audio file the AAF needs is there (embedded audio counts).");
    x += w + 8;
  } else {
    std::string txt = strprintf("%d missing", total - ok);
    double w = g.width(st->fBody, txt) + 8;
    g.text(st->fBody, kRed, txt, x, t, x + w, b, DT_LEFT);
    x += w + 4;
    button(st, g, x, mid - 15, x + 78, mid + 15, "Locate\xe2\x80\xa6", H_LOCATE, 0,
           "Choose the folder that holds the media. All its subfolders are searched, and OLAAF remembers it for the next import.");
    x += 86;
  }
  // copy: one plain checkbox, off by default
  x = chip(st, g, x, mid - 13, "Copy audio into a subfolder", opt.copyToSubdir, true, opt.copyToSubdir ? H_COPY_NO : H_COPY_YES,
           "On: WAV files are copied into the audio subfolder. Off: they stay where they are. MXF, AIFF and embedded audio "
           "always become Broadcast WAV in the subfolder.");
  x += 8;

  // where it goes
  double cancelW = 84, buildW = 190;
  double pr = kR - buildW - cancelW - 16 - 86;
  std::string sub = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
  std::string dest = opt.destDir;
  if (!dest.empty() && dest.back() != '/' && dest.back() != '\\') dest += "/";
  g.box(x, mid - 18, pr, mid + 18, kLine, kPanel);
  g.text(st->fSec, kMute, opt.copyToSubdir ? "Audio goes to" : "Converted audio goes to", x + 8, mid - 16, pr - 8, mid - 2, DT_LEFT);
  // the folder it goes into, shortened from the left so the subfolder name always shows
  size_t maxc = (size_t)std::max(24.0, (pr - x - 16) / 6.2);
  std::string subShown = sub.size() > maxc / 2 ? utf8Head(sub, maxc / 2 - 1) + "\xe2\x80\xa6" : sub;
  std::string shown = shortPath(dest, std::max<size_t>(12, maxc - subShown.size())) + subShown;
  if (st->editKind != 3) g.text(st->fSmall, kInk, shown, x + 8, mid - 2, pr - 8, mid + 15, DT_LEFT);
  st->subRc = devRect(st, x + 6, mid - 2, pr - 6, mid + 15);
  addHit(st, x, mid - 18, pr, mid + 18, H_SUBDIR, 0, "Click to rename the audio subfolder.");
  button(st, g, pr + 6, mid - 15, pr + 80, mid + 15, "Choose\xe2\x80\xa6", H_DEST_CHOOSE, 0,
         "Choose the folder the audio subfolder goes into.");

  int ntr = 0;
  for (auto &tr : s.tracks) {
    if (!tr.import) continue;
    bool any = !tr.created;
    for (auto &c : tr.clips) any = any || !c.excluded;
    ntr += any;
  }
  button(st, g, kR - buildW - 8 - cancelW, mid - 18, kR - buildW - 8, mid + 18, "Cancel", H_CANCEL, 0, "Close without importing.");
  button(st, g, kR - buildW, mid - 18, kR, mid + 18, strprintf("Build session \xc2\xb7 %d track%s", ntr, ntr == 1 ? "" : "s"),
         H_IMPORT, 0, "Import the tracks into REAPER.", true, ntr > 0);
}

void paintFooter(State *st, const Gfx &g) {
  g.fill(kL, kFootY, kR, kFootY + 1, kLine);
  double t = kFootY + 3, b = H - 6;
  const char *v = "V 0.6 beta developed with AI assistance";
  double vw = g.width(st->fTag, v) + 10;
  if (vw < 100 || vw > 400) vw = 240;
  g.box(kR - 22, t + 2, kR, b - 2, kLine, kPanel);
  g.icon(darkTheme() ? "\xe2\x98\xbe" : "\xe2\x98\x80", kR - 11, (t + b) / 2, kInk, kPanel);
  addHit(st, kR - 22, t + 2, kR, b - 2, H_THEME, 0, "Light or dark interface.");
  g.text(st->fTag, kMute, v, kR - 30 - vw, t, kR - 30, b, DT_RIGHT);
  g.text(st->fTag, kMute,
         st->tip ? st->tip : "The square leaves a track out. Drag track names onto a folder, drag events (or use the arrow keys) to move them to another track.",
         kL, t, kR - 40 - vw, b, DT_LEFT);
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
  g.box(px, py, px + pw, py + ph, kLine, kPanel);
  addHit(st, px, py, px + pw, py + ph, H_FOLD_POP, 0, nullptr);
  g.text(st->fSec, kMute, "Folder \xc2\xb7 " + tr.name, px + 10, py + 4, px + pw - 10, py + 26, DT_LEFT);
  double y = py + 30;
  for (int i = 0; i < n; i++, y += rowH) {
    const Folder &f = s.folders[(size_t)i];
    bool cur = tr.group == f.key;
    if (cur) g.fill(px + 1, y, px + pw - 1, y + rowH, kBlueSoft);
    g.box(px + 9, y + 5, px + 25, y + 21, theme().empty, f.color ? rgbToRef(f.color) : kPanel);
    addHit(st, px + 7, y + 3, px + 27, y + 23, H_FOLD_COLOR, i, "Set the folder color.");
    bool renaming = st->editKind == 2 && st->editKey == f.key;
    if (!renaming) g.text(cur ? st->fBold : st->fBody, kInk, f.name, px + 30, y, px + pw - 64, y + rowH, DT_LEFT);
    int nTr = 0;
    for (auto &t : s.tracks) nTr += t.group == f.key;
    g.text(st->fClip, kMute, std::to_string(nTr), px + pw - 84, y, px + pw - 62, y + rowH, DT_RIGHT);
    addHit(st, px + 28, y, px + pw - 60, y + rowH, H_FOLD_ASSIGN, i, "Put the track into this folder.");
    button(st, g, px + pw - 56, y + 3, px + pw - 34, y + rowH - 3, "\xe2\x9c\x8e", H_FOLD_RENAME, i, "Rename this folder.");
    button(st, g, px + pw - 30, y + 3, px + pw - 8, y + rowH - 3, "\xc3\x97", H_FOLD_DELETE, i,
           "Don't create this folder. Its tracks are imported outside any folder.");
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
  g.box(px, py, px + kDpW, py + ph, kLine, kPanel);
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
  g.box(px, py, px + pw, py + ph, kLine, kPanel);
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
  paintHeader(st, g);
  paintToolbar(st, g);
  paintArrange(st, g);
  paintEventBar(st, g);
  paintMediaBar(st, g);
  paintFooter(st, g);
  if (st->folderTrack >= 0 && st->opt->folders) paintFolderPopover(st, g);
  if (st->destTrack >= 0) paintDestPopover(st, g);
  if (st->palTrack >= 0 || !st->palFolder.empty()) paintPalette(st, g);
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

// moves the selection by `delta` tracks in the order the tracks are shown; below the last
// track new tracks are added (outside any folder, so they show up at the bottom)
void moveSelectionBy(State *st, int delta) {
  if (!delta || st->sel.empty()) return;
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
  for (auto &p : byPos) st->imp->moveClips({p.second}, st->order[(size_t)(p.first + delta)], st->moveAutomation);
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
    if (parseTC(v, st->imp->session().fps, &sec)) {
      st->tcAbs = std::fabs(sec);
      if (v.find('-') != std::string::npos) st->tcMinus = true;
      else if (v.find('+') != std::string::npos) st->tcMinus = false;
      st->tip = nullptr;
    } else
      st->tip = "That is not a timecode. Use HH:MM:SS:FF or just the digits.";
  }
  if (apply && kind == 2)
    if (Folder *f = st->imp->session().folder(st->editKey))
      if (!v.empty()) f->name = v;
  if (apply && kind == 3 && !v.empty()) st->opt->subdir = safeFileName(v);
  if (apply && kind == 4 && !v.empty() && st->editTrack >= 0 && st->editTrack < (int)st->imp->session().tracks.size())
    st->imp->session().tracks[(size_t)st->editTrack].name = v;
  ShowWindow(st->tcEdit, SW_HIDE);
  SetFocus(st->hwnd);
  InvalidateRect(st->hwnd, NULL, FALSE);
}

// closes the window: modal -> EndDialog, modeless -> destroy it, then hand the result on
void closeDialog(State *st, bool accepted) {
  st->accepted = accepted;
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

void click(State *st, int x, int y, bool shift) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  const Hit *h = hitAt(st, x, y);
  int id = h ? h->id : H_NONE;
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
    double y = kLT + (ri - st->scrollRow) * kRowH;
    startEdit(st, 2, devRect(st, kL + 26, y + 4, kL + kHdr - 32, y + kRowH - 4), "New folder", key);
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
  case H_MERGE: {
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
  case H_VIDEO: opt.video = !opt.video; break;
  case H_VIDEO_CHOOSE: { // the AAF has no picture (or a chosen one is replaced): pick the file by hand
    std::string path;
    if (st->host->pickFile && st->host->pickFile("OLAAF: choose the video for this AAF", &path) && !path.empty()) {
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
  case H_THEME: setDarkTheme(!darkTheme()); break;
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
      st->peaks.start(s);
      SetTimer(st->hwnd, kPeakTimer, 150, NULL);
    }
    break;
  }
  case H_TC_PLUS: st->tcMinus = false; break;
  case H_TC_MINUS: st->tcMinus = true; break;
  case H_TC_VALUE: startEdit(st, 1, st->tcRc, formatTC(st->tcAbs, s.fps)); return;
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
  case H_EV_UP: moveSelectionBy(st, -1); break;
  case H_EV_DOWN: moveSelectionBy(st, 1); break;
  case H_EV_AUTOMATION: st->moveAutomation = !st->moveAutomation; break;
  case H_EV_RESET:
    keepTrackChoices(st, [&]() { st->imp->setMergeStereo(st->merge); });
    for (auto &t : s.tracks)
      for (auto &c : t.clips) c.mute = false;
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
bool handleKey(State *st, int vk) {
  if (!st || st->editKind) return false;
  bool in = vk == VK_ADD || vk == 0xBB /* VK_OEM_PLUS */;
  bool out = vk == VK_SUBTRACT || vk == 0xBD /* VK_OEM_MINUS */;
#ifndef _WIN32
  in = in || vk == '+' || vk == '=';
  out = out || vk == '-';
#endif
  if (in || out) {
    zoomBy(st, in ? 0.5 : 2.0, (kTlx + kTlr) / 2);
    InvalidateRect(st->hwnd, NULL, FALSE);
    return true;
  }
  if (st->sel.empty()) return false;
  if (vk == VK_UP) moveSelectionBy(st, -1);
  else if (vk == VK_DOWN) moveSelectionBy(st, 1);
  else if (vk == 'M' || vk == 'm') toggleMute(st);
  else if (vk == VK_DELETE || vk == VK_BACK) toggleExcluded(st);
  else return false;
  InvalidateRect(st->hwnd, NULL, FALSE);
  return true;
}

State *g_open = nullptr; // the dialog that is open (for REAPER's keyboard hook)

INT_PTR CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  State *st = (State *)OL_GETUSER(hwnd);
  switch (msg) {
  case WM_INITDIALOG: {
    st = (State *)lp;
    OL_SETUSER(hwnd, st);
    st->hwnd = hwnd;
    g_open = st;
#ifdef _WIN32
    HDC dc = GetDC(hwnd);
    st->base = st->scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(hwnd, dc);
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
    st->peaks.start(st->imp->session()); // waveforms fill in as the files are read
    SetTimer(hwnd, kPeakTimer, 150, NULL);
    return 0;
  }
  case WM_TIMER:
    if (st && wp == kPeakTimer) {
      if (st->peaks.version() != st->peakSeen) InvalidateRect(hwnd, NULL, FALSE);
      if (!st->peaks.busy() && st->peaks.version() == st->peakSeen) KillTimer(hwnd, kPeakTimer);
    }
    return 0;
  case WM_ERASEBKGND: return 1;
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
    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DLGC_WANTARROWS | DLGC_WANTCHARS);
    return TRUE;
#endif
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
  case WM_LBUTTONDOWN: {
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
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
      if (drag == 4 && moved && st->dropRow >= 0) dropTracks(st, st->dropRow);
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
    if (drag == 3) {
      if (moved && st->dragRows) moveSelectionBy(st, st->dragRows);
      else if (st->pressedClip) click(st, p.x, p.y, shiftDown());
      st->dragRows = 0;
      st->tip = nullptr;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    bool onBar = PtInRect(&st->vTrack, p) || PtInRect(&st->hTrack, p);
    if (!drag && !onBar) click(st, p.x, p.y, shiftDown());
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_MOUSEMOVE: {
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
        st->tip = "Release to move the selected events to this track. Below the last track a new one is added.";
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    const Hit *h = hitAt(st, mx, my);
    const char *tip = h ? h->tip : nullptr;
    if (tip != st->tip) {
      st->tip = tip;
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
  case WM_MOUSEWHEEL: {
    int delta = (short)HIWORD(wp);
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

#ifdef OLAAF_DIALOG_TEST
// renders the dialog into any HDC, for checking the layout without REAPER
// palTrack: >= 0 color popover of that track, <= -2 folder popover of track (-2 - n)
void dialogPaintTest(Importer &imp, ImportOptions &opt, HDC dc, int palTrack, const char *tip) {
  static DialogHost host;
  State st;
  st.imp = &imp;
  st.opt = &opt;
  st.host = &host;
  st.hwnd = (HWND)1;
  st.merge = opt.mergeStereo;
  st.tip = tip;
  st.peaks.runNow(imp.session());
  makeFonts(&st);
  double a, b;
  sessionRange(imp.session(), &a, &b);
  st.viewStart = a - (b - a) * 0.02;
  st.viewLen = (b - a) * 1.04;
  st.tcAbs = 36000;
  auto &tr = imp.session().tracks;
  if (!tr.empty() && tr[0].clips.size() > 2) {
    st.sel.insert(tr[0].clips[2].id);
    imp.setExcluded({tr[0].clips[1].id}, true);
    if (tr[0].clips.size() > 3) tr[0].clips[3].mute = true;
    st.edited = true;
  }
  if (getenv("OLAAF_SHOWAUTO")) opt.showAutomation = true;
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
  paint(&st, dc);
}
#endif

// REAPER hands every key press to registered extensions first (otherwise its own shortcuts,
// like M, take them): returns 1 when the dialog used the key, -1 to let the dialog have it, 0 if not ours
int dialogTranslateKey(MSG *msg) {
  State *st = g_open;
  if (!st || !st->hwnd || !msg) return 0;
  if (msg->hwnd != st->hwnd && !IsChild(st->hwnd, msg->hwnd)) return 0;
  if (st->editKind) { // typing in a field: Return applies, Esc cancels
    if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE)) {
      endEdit(st, msg->wParam == VK_RETURN);
      return 1;
    }
    return -1;
  }
  if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
    SendMessage(st->hwnd, WM_COMMAND, IDCANCEL, 0);
    return 1;
  }
  if (msg->message == WM_KEYDOWN && handleKey(st, (int)msg->wParam)) return 1;
  if (msg->message == WM_KEYDOWN || msg->message == WM_CHAR || msg->message == WM_KEYUP) return -1;
  return 0;
}

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
