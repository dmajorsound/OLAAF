// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The import dialog is drawn by hand (like the other dmajorsound plugins), so it looks the same
// on macOS and Windows: square controls, explanations bottom left, light and dark mode.
//
// Layout: timeline on the left with the selected event below it; the track list (row by row
// next to the timeline) and all options on the right.

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "dialog.h"
#include "theme.h"
#include "logo.h"
#include "logo.h"
#include "../core/util.h"

#ifndef _WIN32
#include "../../third_party/reaper-sdk/WDL/swell/swell-dlggen.h"
#ifndef SWELL_DLG_WS_DEFAULT_SCALING
#define SWELL_DLG_WS_DEFAULT_SCALING 0
#endif
#define IDD_OLAAF_IMPORT 4712
SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_OLAAF_IMPORT, SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE | SWELL_DLG_WS_OPAQUE,
                                   "olAAF", 1180, 800, 1.0)
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
const COLORREF kWhite = RGB(0xff, 0xff, 0xff);

const uint32_t kPalette[10] = {0x5580d4, 0x1F9A8A, 0x4E8F3A, 0xB8962E, 0xD9822B,
                               0xC8423B, 0xB0457E, 0x7A5AC8, 0x6B7480, 0x2B3440};

COLORREF rgbToRef(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// geometry (logical pixels)
const double kLeft = 16, kRight = W - 16;
const double kListW = 380;                      // right column
const double kColR = kRight - kListW;           // right column starts
const double kTlR = kColR - 12;                 // timeline panel ends
const double kTop = 114, kHead = 36, kRuler = 24, kRowH = 30, kSb = 12;
const int kRows = 8;
const double kRowsTop = kTop + kHead + kRuler;   // first row
const double kRowsBot = kRowsTop + kRows * kRowH;
const double kBandBot = kRowsBot + kSb + 2;      // bottom of the timeline / list panels
const double kLower = kBandBot + 12;             // cards below

enum HitId {
  H_NONE, H_THEME, H_VIDEO, H_MARKERS, H_FOLDERS, H_ZOOM_FIT, H_TRACK_CHECK, H_TRACK_COLOR, H_SEL_ALL, H_SEL_NONE,
  H_ZOOM_IN, H_ZOOM_OUT, H_MERGE, H_FADES, H_GAIN, H_COPY_YES, H_COPY_NO, H_SUBDIR, H_DEST, H_DEST_CHOOSE, H_LOCATE,
  H_CANCEL, H_IMPORT, H_TC_ON, H_TC_PLUS, H_TC_MINUS, H_TC_VALUE, H_TC_CURSOR, H_PAL, H_PAL_CUSTOM, H_PAL_NONE,
  H_LIST, H_TIMELINE, H_EV_REMOVE, H_EV_RESTORE, H_EV_UP, H_EV_DOWN, H_EV_AUTOMATION, H_EV_RESET
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
  int row; // visible row
};

struct State {
  Importer *imp = nullptr;
  ImportOptions *opt = nullptr;
  const DialogHost *host = nullptr;
  HWND hwnd = nullptr;
  double scale = 1.0;
  std::vector<Hit> hits;
  std::vector<ClipHit> clipHits;
  const char *tip = nullptr;
  int scrollRow = 0;
  double viewStart = 0, viewLen = 60;
  bool tcOn = false, tcMinus = true;
  double tcAbs = 0;
  int palTrack = -1;
  RECT vTrack{}, vThumb{}, hTrack{}, hThumb{}, rowsRc{};
  int rowsTotal = 0;
  double rangeA = 0, rangeB = 1;
  int drag = 0; // 1 vertical thumb, 2 horizontal thumb, 3 events
  int dragFromX = 0, dragFromY = 0;
  double dragValue = 0;
  bool dragMoved = false;
  int dragRows = 0;       // rows the events are being dragged by
  int pressedClip = 0;    // clip under the mouse at button down
  std::set<int> sel;      // selected clip ids
  bool moveAutomation = true;
  bool edited = false;
  bool merge = true;
  bool accepted = false;
  HFONT fTheme = nullptr, fCheck = nullptr, fClip = nullptr, fTitle = nullptr, fTitleThin = nullptr, fTag = nullptr,
        fBody = nullptr, fBold = nullptr, fSmall = nullptr, fSec = nullptr, fLogo = nullptr;
};

HFONT makeFont(double px, bool bold, double scale) {
#ifdef _WIN32
  const char *face = "Segoe UI";
#elif defined(__APPLE__)
  const char *face = "Helvetica Neue";
#else
  const char *face = "Sans";
#endif
  return CreateFont(-(int)std::lround(px * scale), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face);
}

void makeFonts(State *st) {
  double s = st->scale;
  st->fTheme = makeFont(17, false, s);
  st->fCheck = makeFont(13, true, s);
  st->fClip = makeFont(10, false, s);
  st->fTitle = makeFont(19, true, s);
  st->fTitleThin = makeFont(19, false, s);
  st->fTag = makeFont(10.5, false, s);
  st->fBody = makeFont(13, false, s);
  st->fBold = makeFont(13, true, s);
  st->fSmall = makeFont(11.5, false, s);
  st->fSec = makeFont(10.5, true, s);
  st->fLogo = makeFont(11, false, s);
}

struct Gfx {
  HDC dc;
  double s;
  int S(double v) const { return (int)std::lround(v * s); }
  void fill(double l, double t, double r, double b, COLORREF c) const {
    RECT rc = {S(l), S(t), S(r), S(b)};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
  }
  void box(double l, double t, double r, double b, COLORREF line, COLORREF in) const {
    fill(l, t, r, b, line);
    RECT rc = {S(l) + 1, S(t) + 1, S(r) - 1, S(b) - 1};
    HBRUSH br = CreateSolidBrush(in);
    FillRect(dc, &rc, br);
    DeleteObject(br);
  }
  void text(HFONT f, COLORREF c, const std::string &str, double l, double t, double r, double b, int align) const {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT rc = {S(l), S(t), S(r), S(b)};
    DrawText(dc, str.c_str(), -1, &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
  }
  double width(HFONT f, const std::string &str) const {
    HGDIOBJ old = SelectObject(dc, f);
    RECT rc = {0, 0, 4000, 100};
    DrawText(dc, str.c_str(), -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return (rc.right - rc.left) / s;
  }
  void line(double x1, double y1, double x2, double y2, COLORREF c) const {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, S(x1), S(y1), NULL);
    LineTo(dc, S(x2), S(y2));
    SelectObject(dc, old);
    DeleteObject(p);
  }
};

void addHit(State *st, double l, double t, double r, double b, int id, int idx, const char *tip) {
  Hit h;
  h.r = {(LONG)std::lround(l * st->scale), (LONG)std::lround(t * st->scale), (LONG)std::lround(r * st->scale),
         (LONG)std::lround(b * st->scale)};
  h.id = id;
  h.idx = idx;
  h.tip = tip;
  st->hits.push_back(h);
}

void checkbox(State *st, const Gfx &g, double x, double y, bool on, int id, int idx, const char *tip) {
  if (on) {
    g.box(x, y, x + 16, y + 16, kBlue, kBlue);
    g.text(st->fCheck, kWhite, "\xe2\x9c\x93", x, y, x + 16, y + 16, DT_CENTER);
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
    g.text(st->fBody, enabled ? kInk : kMute, label, l, t, r, b, DT_CENTER);
  }
  if (enabled) addHit(st, l, t, r, b, id, idx, tip);
}

void segmented(State *st, const Gfx &g, double l, double t, double w, double h, const char *a, const char *b,
               bool firstOn, int idA, int idB, const char *tip) {
  g.box(l, t, l + w, t + h, kLine, kPanel);
  double m = l + w / 2;
  if (firstOn) {
    g.fill(l + 2, t + 2, m - 1, t + h - 2, kBlue);
    g.text(st->fBold, kWhite, a, l, t, m, t + h, DT_CENTER);
    g.text(st->fBody, kMute, b, m, t, l + w, t + h, DT_CENTER);
  } else {
    g.fill(m + 1, t + 2, l + w - 2, t + h - 2, kBlue);
    g.text(st->fBody, kMute, a, l, t, m, t + h, DT_CENTER);
    g.text(st->fBold, kWhite, b, m, t, l + w, t + h, DT_CENTER);
  }
  addHit(st, l, t, m, t + h, idA, 0, tip);
  addHit(st, m, t, l + w, t + h, idB, 0, tip);
}

void card(State *st, const Gfx &g, double l, double t, double r, double b, const char *title) {
  g.box(l, t, r, b, kLine, kPanel);
  if (title) g.text(st->fSec, kMute, title, l + 14, t + 6, r - 14, t + 24, DT_LEFT);
}

double niceStep(double secPerPx) {
  static const double steps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
  for (double s : steps)
    if (s / secPerPx >= 90) return s;
  return 7200;
}

double tcOffset(const State *st) { return st->tcOn ? (st->tcMinus ? -st->tcAbs : st->tcAbs) : 0.0; }

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

// ------------------------------------------------------------------ painting

void paintHeader(State *st, const Gfx &g, double cw) {
  Session &s = st->imp->session();
  g.fill(16, 18, 46, 48, kBlue);
  g.fill(23, 25, 33, 28.5, kWhite);
  g.fill(27, 31, 39, 34.5, kWhite);
  g.fill(23, 37, 30, 40.5, kWhite);
  g.text(st->fTitle, kInk, "olAAF", 56, 10, 400, 36, DT_LEFT);
  double tw = g.width(st->fTitle, "olAAF");
  if (tw < 20 || tw > 200) tw = 55;
  g.text(st->fTitleThin, kMute, "\xe2\x80\x93 AAF Import for Reaper", 56 + tw + 6, 10, 600, 36, DT_LEFT);
  g.text(st->fTag, kMute, "Do you wanna build a Reaper Session?", 56, 34, 400, 50, DT_LEFT);
  g.box(cw - 48, 16, cw - 16, 48, kLine, kPanel);
  g.text(st->fTheme, kInk, darkTheme() ? "\xe2\x98\xbe" : "\xe2\x98\x80", cw - 48, 16, cw - 16, 48, DT_CENTER);
  addHit(st, cw - 48, 16, cw - 16, 48, H_THEME, 0, "Light or dark interface.");

  // file bar with the timecode offset
  double fy = 60, fb = 104;
  g.box(16, fy, cw - 16, fb, kLine, kPanel);
  g.text(st->fBold, kInk, baseName(s.aafPath), 30, fy + 4, 520, fy + 23, DT_LEFT);
  g.text(st->fSmall, kMute,
         strprintf("%s  \xc2\xb7  %d Hz \xc2\xb7 %d bit \xc2\xb7 %g fps \xc2\xb7 Start %s \xc2\xb7 %zu tracks",
                   s.product.c_str(), s.sampleRate, s.bits, s.fps, formatTC(s.start, s.fps).c_str(), s.tracks.size()),
         30, fy + 22, 560, fy + 40, DT_LEFT);
  double x = cw - 16 - 610;
  checkbox(st, g, x, fy + 14, st->tcOn, H_TC_ON, 0, "Timecode offset: shifts every clip by this amount on import.");
  g.text(st->fBody, kInk, "Timecode offset", x + 24, fy + 8, x + 140, fy + 36, DT_LEFT);
  segmented(st, g, x + 140, fy + 8, 56, 28, "+", "\xe2\x88\x92", !st->tcMinus, H_TC_PLUS, H_TC_MINUS,
            "Add or subtract the offset. The ruler shows the shifted timecode.");
  g.box(x + 204, fy + 8, x + 304, fy + 36, kLine, kPanel);
  g.text(st->fBody, kInk, formatTC(st->tcAbs, s.fps), x + 204, fy + 8, x + 304, fy + 36, DT_CENTER);
  addHit(st, x + 204, fy + 8, x + 304, fy + 36, H_TC_VALUE, 0, "Click to type the offset as HH:MM:SS:FF.");
  g.text(st->fSmall, kMute, "\xe2\x86\x92 Starts at", x + 312, fy + 8, x + 380, fy + 36, DT_LEFT);
  g.text(st->fBold, kInk, formatTC(s.firstClip() + tcOffset(st), s.fps), x + 380, fy + 8, x + 476, fy + 36, DT_LEFT);
  button(st, g, x + 484, fy + 8, x + 594, fy + 36, "Edit cursor", H_TC_CURSOR, 0,
         "Sets the offset so the AAF starts at the REAPER edit cursor.");
}

void paintTimeline(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double l = kLeft, r = kTlR, t = kTop;
  g.box(l, t, r, kBandBot, kLine, kPanel);
  g.fill(l + 1, t + kHead, r - 1, t + kHead + 1, kLine);
  g.text(st->fSec, kMute, "TIMELINE", l + 14, t, l + 100, t + kHead, DT_LEFT);
  int removed = 0;
  for (auto &tr : s.tracks)
    for (auto &c : tr.clips) removed += c.excluded;
  std::string info = st->sel.empty() ? "Click an event to select it" : strprintf("%zu selected", st->sel.size());
  if (removed) info += strprintf("  \xc2\xb7  %d removed", removed);
  g.text(st->fSmall, kMute, info, l + 100, t, r - 140, t + kHead, DT_LEFT);
  button(st, g, r - 124, t + 6, r - 76, t + 30, "Fit", H_ZOOM_FIT, 0, "Show the whole project.");
  button(st, g, r - 70, t + 6, r - 42, t + 30, "\xe2\x88\x92", H_ZOOM_OUT, 0, "Zoom out. Shift + mouse wheel moves through time.");
  button(st, g, r - 36, t + 6, r - 8, t + 30, "+", H_ZOOM_IN, 0, "Zoom in. Shift + mouse wheel moves through time.");

  double tlx = l + 10, tlr = r - 10, tlw = tlr - tlx;
  double a, b;
  sessionRange(s, &a, &b);
  st->rangeA = a - (b - a) * 0.02;
  st->rangeB = b + (b - a) * 0.02;
  double maxStart = std::max(st->rangeA, st->rangeB - st->viewLen);
  st->viewStart = std::max(std::min(st->viewStart, maxStart), std::min(st->rangeA, maxStart));
  double spp = st->viewLen / tlw;

  // ruler
  double ry = t + kHead + 1;
  g.fill(l + 1, ry, r - 1, kRowsTop, theme().track);
  g.fill(l + 1, kRowsTop - 1, r - 1, kRowsTop, kLine);
  double step = niceStep(spp), off = tcOffset(st);
  for (double tt = std::floor((st->viewStart + off) / step) * step; tt <= st->viewStart + off + st->viewLen; tt += step) {
    double px = tlx + (tt - off - st->viewStart) / spp;
    if (px < tlx || px > tlr) continue;
    g.fill(px, ry, px + 1, kRowsBot, kSoft);
    g.fill(px, ry, px + 1, kRowsTop, kLine);
    g.text(st->fSmall, kMute, formatTC(tt, s.fps), px + 4, ry, std::min(px + 100, tlr), kRowsTop, DT_LEFT);
  }
  // markers
  if (opt.markers)
    for (auto &m : s.markers) {
      double px = tlx + (m.pos - st->viewStart) / spp;
      if (px < tlx || px > tlr) continue;
      COLORREF mc = m.color ? rgbToRef(m.color) : kRed;
      g.fill(px, ry, px + 2, kRowsBot, mc);
    }

  // rows
  int ntr = (int)s.tracks.size();
  st->rowsTotal = ntr;
  st->scrollRow = std::max(0, std::min(st->scrollRow, std::max(0, ntr - kRows)));
  st->rowsRc = {(LONG)g.S(tlx), (LONG)g.S(kRowsTop), (LONG)g.S(tlr), (LONG)g.S(kRowsBot)};
  st->clipHits.clear();
  addHit(st, tlx, kRowsTop, tlr, kRowsBot, H_TIMELINE, 0,
         "Click selects, Shift-click adds, drag up or down moves events to another track.");
  int dragDelta = (st->drag == 3 && st->dragMoved) ? st->dragRows : 0;
  for (int row = 0; row < kRows; row++) {
    int ti = st->scrollRow + row;
    double y = kRowsTop + row * kRowH;
    g.fill(l + 1, y + kRowH - 1, r - 1, y + kRowH, kSoft);
    if (ti >= ntr) continue;
    Track &tr = s.tracks[(size_t)ti];
    COLORREF tc = tr.color ? rgbToRef(tr.color) : RGB(0x7f, 0x9c, 0xd8);
    for (auto &c : tr.clips) {
      double x0 = tlx + (c.pos - st->viewStart) / spp, x1 = tlx + (c.pos + c.len - st->viewStart) / spp;
      if (x1 < tlx || x0 > tlr) continue;
      x0 = std::max(x0, tlx);
      x1 = std::min(x1, tlr);
      if (x1 - x0 < 1) x1 = x0 + 1;
      bool selected = st->sel.count(c.id) > 0;
      double y0 = y + 4, y1 = y + kRowH - 5;
      if (selected && dragDelta) {
        int target = ti + dragDelta;
        if (target >= st->scrollRow && target < st->scrollRow + kRows) {
          double gy = kRowsTop + (target - st->scrollRow) * kRowH;
          g.box(x0, gy + 4, x1, gy + kRowH - 5, kBlue, kBlueSoft);
        }
      }
      if (c.excluded) {
        g.box(x0, y0, x1, y1, selected ? kInk : kLine, kPanel);
        g.line(x0, y1, x1, y0, kLine);
      } else {
        COLORREF cc = tr.import ? tc : theme().off;
        if (selected) g.fill(x0 - 1, y0 - 2, x1 + 1, y1 + 2, kInk);
        g.fill(x0, y0, x1, y1, cc);
        double fi = c.fadeIn.len / spp, fo = c.fadeOut.len / spp;
        if (opt.fades && fi > 2) g.line(x0, y1, std::min(x1, x0 + fi), y0, kInk);
        if (opt.fades && fo > 2) g.line(std::max(x0, x1 - fo), y0, x1, y1, kInk);
      }
      if (x1 - x0 > 40 && !c.name.empty())
        g.text(st->fClip, c.excluded ? kMute : (tr.import ? kWhite : kMute), c.name, x0 + 4, y0 + 1, x1 - 3, y1 - 1, DT_LEFT);
      ClipHit h;
      h.r = {(LONG)g.S(x0), (LONG)g.S(y0), (LONG)g.S(x1), (LONG)g.S(y1)};
      h.id = c.id;
      h.row = row;
      st->clipHits.push_back(h);
    }
  }

  // horizontal scrollbar
  double hl = tlx, hr = tlr, ht = kRowsBot + 1, hb = kRowsBot + 1 + kSb;
  g.fill(l + 1, kRowsBot, r - 1, kRowsBot + 1, kLine);
  g.fill(hl, ht, hr, hb, theme().track);
  double total = st->rangeB - st->rangeA;
  double frac = std::min(1.0, st->viewLen / std::max(1e-9, total));
  double tw = std::max(24.0, (hr - hl) * frac);
  double pos = total > st->viewLen ? (st->viewStart - st->rangeA) / (total - st->viewLen) : 0;
  pos = std::max(0.0, std::min(1.0, pos));
  double tx = hl + (hr - hl - tw) * pos;
  g.fill(tx + 2, ht + 3, tx + tw - 2, hb - 2, frac < 1.0 ? theme().thumb : kSoft);
  st->hTrack = {(LONG)g.S(hl), (LONG)g.S(ht), (LONG)g.S(hr), (LONG)g.S(hb)};
  st->hThumb = {(LONG)g.S(tx), (LONG)g.S(ht), (LONG)g.S(tx + tw), (LONG)g.S(hb)};
  addHit(st, hl, ht, hr, hb, H_NONE, 0, "Drag to move through time, or use + and \xe2\x88\x92 to zoom.");
}

void paintTrackList(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double l = kColR, r = kRight, t = kTop;
  g.box(l, t, r, kBandBot, kLine, kPanel);
  g.fill(l + 1, t + kHead, r - 1, t + kHead + 1, kLine);
  int nsel = 0;
  for (auto &tr : s.tracks) nsel += tr.import;
  g.text(st->fSec, kMute, "TRACKS", l + 14, t, l + 80, t + kHead, DT_LEFT);
  g.text(st->fSmall, kMute, strprintf("%d of %zu", nsel, s.tracks.size()), l + 80, t, l + 160, t + kHead, DT_LEFT);
  button(st, g, r - 140, t + 6, r - 62, t + 30, "Select all", H_SEL_ALL, 0, "Select every track.");
  button(st, g, r - 56, t + 6, r - 8, t + 30, "None", H_SEL_NONE, 0, "Deselect every track.");
  double ry = t + kHead + 1;
  g.fill(l + 1, ry, r - 1, kRowsTop, theme().track);
  g.fill(l + 1, kRowsTop - 1, r - 1, kRowsTop, kLine);
  g.text(st->fSmall, kMute, "Name", l + 60, ry, l + 200, kRowsTop, DT_LEFT);
  if (opt.folders) g.text(st->fSmall, kMute, "Folder", l + 230, ry, l + 300, kRowsTop, DT_LEFT);
  g.text(st->fSmall, kMute, "Events", l + 300, ry, r - kSb - 10, kRowsTop, DT_RIGHT);
  addHit(st, l, kRowsTop, r, kRowsBot, H_LIST, 0, "The square sets the track color. Mouse wheel scrolls.");

  int ntr = (int)s.tracks.size();
  for (int row = 0; row < kRows; row++) {
    int ti = st->scrollRow + row;
    double y = kRowsTop + row * kRowH;
    g.fill(l + 1, y + kRowH - 1, r - 1, y + kRowH, kSoft);
    if (ti >= ntr) continue;
    Track &tr = s.tracks[(size_t)ti];
    bool on = tr.import;
    checkbox(st, g, l + 12, y + 7, on, H_TRACK_CHECK, ti, "Import this track or leave it out.");
    if (tr.color) g.box(l + 36, y + 7, l + 52, y + 23, RGB(0x99, 0x99, 0x99), rgbToRef(tr.color));
    else {
      g.box(l + 36, y + 7, l + 52, y + 23, theme().empty, kPanel);
      g.line(l + 38, y + 21, l + 50, y + 9, theme().empty);
    }
    addHit(st, l + 34, y + 5, l + 54, y + 25, H_TRACK_COLOR, ti, "Set the track color.");
    double nameR = opt.folders ? l + 222 : l + 290;
    if (tr.channels >= 2) {
      const char *badge = tr.channels == 2 ? (tr.mergedFrom.empty() ? "ST" : "L+R") : "MC";
      g.box(nameR - 34, y + 8, nameR - 4, y + 22, theme().blueLine, kBlueSoft);
      g.text(st->fSmall, kBlue, badge, nameR - 34, y + 8, nameR - 4, y + 22, DT_CENTER);
      nameR -= 38;
    }
    COLORREF nameC = on ? kInk : kMute;
    if (!tr.mergedFrom.empty()) {
      g.text(st->fBody, nameC, tr.name, l + 60, y, nameR, y + 17, DT_LEFT);
      g.text(st->fClip, kMute, tr.mergedFrom, l + 60, y + 16, nameR, y + 29, DT_LEFT);
    } else
      g.text(st->fBody, nameC, tr.name, l + 60, y, nameR, y + kRowH, DT_LEFT);
    if (opt.folders) {
      std::string grp = trackGroup(tr.name);
      g.text(st->fClip, rgbToRef(groupColor(grp)), grp, l + 230, y, l + 300, y + kRowH, DT_LEFT);
    }
    int active = 0;
    for (auto &c : tr.clips) active += !c.excluded;
    g.text(st->fSmall, kMute, std::to_string(active), l + 300, y, r - kSb - 10, y + kRowH, DT_RIGHT);
  }

  // vertical scrollbar (shared by list and timeline)
  double vl = r - kSb - 1, vr = r - 1, vt = kRowsTop, vb = kRowsBot;
  g.fill(vl, vt, vr, vb, theme().track);
  g.fill(vl, vt, vl + 1, vb, kSoft);
  double th = ntr > kRows ? std::max(24.0, (vb - vt) * kRows / ntr) : (vb - vt);
  double tp = ntr > kRows ? vt + (vb - vt - th) * st->scrollRow / (ntr - kRows) : vt;
  g.fill(vl + 3, tp + 2, vr - 2, tp + th - 2, ntr > kRows ? theme().thumb : kSoft);
  st->vTrack = {(LONG)g.S(vl), (LONG)g.S(vt), (LONG)g.S(vr), (LONG)g.S(vb)};
  st->vThumb = {(LONG)g.S(vl), (LONG)g.S(tp), (LONG)g.S(vr), (LONG)g.S(tp + th)};
  addHit(st, vl, vt, vr, vb, H_NONE, 0, "Drag to scroll through the tracks.");
}

void paintEvent(State *st, const Gfx &g, double ch) {
  Session &s = st->imp->session();
  double l = kLeft, r = kTlR, t = kLower, b = ch - 160;
  card(st, g, l, t, r, b, "EVENT");
  std::vector<int> ids(st->sel.begin(), st->sel.end());
  if (ids.empty()) {
    g.text(st->fBody, kMute, "No event selected.", l + 14, t + 30, r - 14, t + 52, DT_LEFT);
    g.text(st->fSmall, kMute, "Click an event in the timeline. Shift-click selects more.", l + 14, t + 54, r - 14, t + 72, DT_LEFT);
    g.text(st->fSmall, kMute, "Drag events up or down to put them on another track.", l + 14, t + 72, r - 14, t + 90, DT_LEFT);
  } else {
    int ti = -1;
    Clip *c = st->imp->findClip(ids[0], &ti);
    bool anyIn = false, anyOut = false;
    for (int id : ids)
      if (Clip *k = st->imp->findClip(id)) (k->excluded ? anyOut : anyIn) = true;
    if (c && ids.size() == 1) {
      g.text(st->fBold, kInk, c->name.empty() ? "(no name)" : c->name, l + 14, t + 28, r - 130, t + 48, DT_LEFT);
      double fo = tcOffset(st);
      struct Col { const char *k; std::string v; } cols[5] = {
          {"Track", ti >= 0 ? s.tracks[(size_t)ti].name : "-"},
          {"Start", formatTC(c->pos + fo, s.fps)},
          {"Length", formatTC(c->len, s.fps)},
          {"Gain", dbText(c->gain) + (c->gainEnv.empty() ? "" : " + curve")},
          {"Fades", strprintf("%.2f s / %.2f s", c->fadeIn.len, c->fadeOut.len)}};
      double cx = l + 14, cwid = (r - l - 28) / 5;
      for (auto &col : cols) {
        g.text(st->fSmall, kMute, col.k, cx, t + 50, cx + cwid - 8, t + 66, DT_LEFT);
        g.text(st->fBody, c->excluded ? kMute : kInk, col.v, cx, t + 66, cx + cwid - 8, t + 86, DT_LEFT);
        cx += cwid;
      }
    } else {
      std::set<int> trs;
      for (int id : ids) {
        int k = -1;
        if (st->imp->findClip(id, &k)) trs.insert(k);
      }
      g.text(st->fBold, kInk, strprintf("%zu events on %zu track%s", ids.size(), trs.size(), trs.size() == 1 ? "" : "s"),
             l + 14, t + 28, r - 130, t + 48, DT_LEFT);
      g.text(st->fSmall, kMute, anyOut && anyIn ? "Some of them are removed from the import." : anyOut ? "All of them are removed from the import." : "",
             l + 14, t + 50, r - 14, t + 68, DT_LEFT);
    }
    double by = b - 40;
    if (anyIn)
      button(st, g, l + 14, by, l + 174, by + 28, "Remove from import", H_EV_REMOVE, 0,
             "Leaves the selected events out. They stay visible here and can be restored.");
    else
      button(st, g, l + 14, by, l + 174, by + 28, "Restore", H_EV_RESTORE, 0, "Puts the selected events back into the import.");
    button(st, g, l + 182, by, l + 280, by + 28, "\xe2\x96\xb2 Track up", H_EV_UP, 0, "Moves the selected events one track up.");
    button(st, g, l + 286, by, l + 400, by + 28, "\xe2\x96\xbc Track down", H_EV_DOWN, 0, "Moves the selected events one track down.");
    checkbox(st, g, l + 414, by + 6, st->moveAutomation, H_EV_AUTOMATION, 0,
             "Volume and pan automation under a moved event goes with it to the new track.");
    g.text(st->fBody, kInk, "Move automation along", l + 438, by, r - 14, by + 28, DT_LEFT);
  }
  if (st->edited)
    button(st, g, r - 116, t + 6, r - 14, t + 28, "Reset edits", H_EV_RESET, 0,
           "Undo every remove and move made in this dialog.");
}

void paintStatus(State *st, const Gfx &g, double ch) {
  Session &s = st->imp->session();
  double l = kLeft, r = kTlR, t = ch - 148, b = ch - 56;
  card(st, g, l, t, r, b, "MEDIA STATUS");
  int total = 0, ok = 0, embedded = 0;
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
    if (m.kind == MediaKind::Embedded) embedded++;
    if (m.kind == MediaKind::Embedded || fileExists(m.foundPath)) ok++;
  }
  std::string emb = embedded ? strprintf(" (%d embedded)", embedded) : "";
  g.text(st->fBody, kInk, strprintf("%d of %d audio files found%s", ok, total, emb.c_str()), l + 14, t + 28, r - 220, t + 50, DT_LEFT);
  if (ok < total) {
    g.text(st->fBody, kRed, strprintf("%d missing \xe2\x80\x93 they import as offline items", total - ok), l + 14, t + 52, r - 220, t + 74, DT_LEFT);
    button(st, g, r - 200, t + 34, r - 14, t + 62, "Locate media folder\xe2\x80\xa6", H_LOCATE, 0,
           "Choose the folder that holds the media; subfolders are searched too.");
  } else
    g.text(st->fBody, theme().green, "Everything is there.", l + 14, t + 52, r - 220, t + 74, DT_LEFT);
}

void paintOptions(State *st, const Gfx &g, double ch) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  double l = kColR, r = kRight;
  double t = kLower;
  card(st, g, l, t, r, t + 112, "IMPORT");
  std::string vLabel = s.video.empty() ? "Video (none)" : "Video";
  if (!s.video.empty() && !fileExists(s.video[0].foundPath)) vLabel += " (missing)";
  std::string mLabel = s.markers.empty() ? "Markers (none)" : strprintf("Markers (%zu)", s.markers.size());
  struct Opt { std::string label; bool on; int id; const char *tip; bool enabled; } opts[6] = {
      {"Merge .L / .R", st->merge, H_MERGE, "Two mono tracks named .L and .R whose clips line up become one stereo track. Switching this resets event edits.", true},
      {"Fades", opt.fades, H_FADES, "Fade ins, fade outs and crossfades from the AAF.", true},
      {"Clip gain", opt.clipGain, H_GAIN, "Clip gain becomes item volume, clip gain automation a take volume envelope.", true},
      {vLabel, opt.video && !s.video.empty(), H_VIDEO, "The picture the AAF points to, on a VIDEO track at the top.", !s.video.empty()},
      {mLabel, opt.markers && !s.markers.empty(), H_MARKERS, "Markers from the edit become REAPER markers.", !s.markers.empty()},
      {"Folder tracks", opt.folders, H_FOLDERS, "Groups the tracks into DIALOG, FOLEY, FX, ATMO, MUSIC and OTHER folders.", true}};
  double colW = (r - l) / 2;
  for (int i = 0; i < 6; i++) {
    double x = l + 14 + (i / 3) * colW, y = t + 28 + (i % 3) * 27;
    if (opts[i].enabled) checkbox(st, g, x, y + 4, opts[i].on, opts[i].id, 0, opts[i].tip);
    else g.box(x, y + 4, x + 16, y + 20, kSoft, kSoft);
    g.text(st->fBody, opts[i].enabled ? kInk : kMute, opts[i].label, x + 24, y, x + colW - 20, y + 24, DT_LEFT);
  }

  t = kLower + 124;
  card(st, g, l, t, r, ch - 104, "MEDIA");
  g.text(st->fBody, kInk, "Copy into subfolder", l + 14, t + 26, l + 200, t + 52, DT_LEFT);
  segmented(st, g, r - 134, t + 26, 120, 26, "Yes", "No", opt.copyToSubdir, H_COPY_YES, H_COPY_NO,
            "Yes: WAV files are copied into the subfolder. No: they stay where they are. MXF, AIFF and embedded audio always become Broadcast WAV.");
  std::string sub = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
  g.box(l + 14, t + 60, r - 14, t + 86, kLine, opt.copyToSubdir ? kPanel : kSoft);
  g.text(st->fBody, opt.copyToSubdir ? kInk : kMute, sub, l + 22, t + 60, r - 22, t + 86, DT_LEFT);
  addHit(st, l + 14, t + 60, r - 14, t + 86, H_SUBDIR, 0, "Click to rename the audio folder.");
  g.box(l + 14, t + 94, r - 104, t + 120, kLine, kPanel);
  g.text(st->fBody, kInk, opt.destDir, l + 22, t + 94, r - 110, t + 120, DT_LEFT);
  addHit(st, l + 14, t + 94, r - 104, t + 120, H_DEST, 0, "Destination folder. Click to type a path.");
  button(st, g, r - 96, t + 94, r - 14, t + 120, "Choose\xe2\x80\xa6", H_DEST_CHOOSE, 0, "Choose the folder the audio is written to.");

  int ntr = 0;
  for (auto &tr : s.tracks) ntr += tr.import;
  double by = ch - 92;
  button(st, g, l, by, l + 120, by + 36, "Cancel", H_CANCEL, 0, "Close without importing.");
  button(st, g, l + 130, by, r, by + 36, strprintf("Import %d track%s", ntr, ntr == 1 ? "" : "s"), H_IMPORT, 0,
         "Import the selected tracks into REAPER.", true, ntr > 0);
}

void paintPalette(State *st, const Gfx &g) {
  Session &s = st->imp->session();
  if (st->palTrack < st->scrollRow || st->palTrack >= st->scrollRow + kRows) return;
  double py = kRowsTop + (st->palTrack - st->scrollRow + 1) * kRowH;
  if (py + 130 > kBandBot) py -= kRowH + 130;
  double px = kColR + 30;
  g.box(px, py, px + 176, py + 126, kLine, kPanel);
  g.text(st->fSec, kMute, "COLOR \xc2\xb7 " + s.tracks[(size_t)st->palTrack].name, px + 10, py + 4, px + 170, py + 24, DT_LEFT);
  for (int i = 0; i < 10; i++) {
    double sx = px + 10 + (i % 5) * 32, sy = py + 28 + (i / 5) * 30;
    bool cur = s.tracks[(size_t)st->palTrack].color == kPalette[i];
    g.box(sx, sy, sx + 24, sy + 24, cur ? kInk : RGB(0x99, 0x99, 0x99), rgbToRef(kPalette[i]));
    addHit(st, sx, sy, sx + 24, sy + 24, H_PAL, i, "Pick this color.");
  }
  button(st, g, px + 10, py + 92, px + 86, py + 116, "Custom\xe2\x80\xa6", H_PAL_CUSTOM, 0, "Any color.");
  button(st, g, px + 92, py + 92, px + 166, py + 116, "None", H_PAL_NONE, 0, "REAPER's default color.");
}

void paint(State *st, HDC dc) {
  Gfx g{dc, st->scale};
  st->hits.clear();
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  double cw = cr.right / st->scale, ch = cr.bottom / st->scale;
  g.fill(0, 0, cw, ch, kFace);
  paintHeader(st, g, cw);
  paintTimeline(st, g);
  paintTrackList(st, g);
  paintEvent(st, g, ch);
  paintStatus(st, g, ch);
  paintOptions(st, g, ch);
  if (st->palTrack >= 0) paintPalette(st, g);
  g.text(st->fSmall, kMute, st->tip ? st->tip : "Pick the tracks and events to import, check the options, then Import.",
         16, ch - 46, cw - 16, ch - 28, DT_LEFT);
  g.fill(16, ch - 26, cw - 16, ch - 25, kLine);
  g.text(st->fTag, kMute, "V 0.4 beta developed with AI assistance by dmajorsound", 16, ch - 22, cw - 200, ch - 6, DT_LEFT);
  drawDmajorLogo(dc, g.S(cw - 16), g.S(ch - 14), g.S(15), kInk, st->fLogo);
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
  if (!PtInRect(&st->rowsRc, p)) return nullptr;
  for (auto it = st->clipHits.rbegin(); it != st->clipHits.rend(); ++it)
    if (PtInRect(&it->r, p)) return &*it;
  return nullptr;
}

std::vector<int> selection(State *st) { return std::vector<int>(st->sel.begin(), st->sel.end()); }

void moveSelectionBy(State *st, int delta) {
  if (!delta || st->sel.empty()) return;
  Session &s = st->imp->session();
  int lo = 1 << 30, hi = -1;
  std::vector<std::pair<int, int>> byTrack; // (track, id)
  for (int id : st->sel) {
    int t = -1;
    if (st->imp->findClip(id, &t)) {
      lo = std::min(lo, t);
      hi = std::max(hi, t);
      byTrack.push_back({t, id});
    }
  }
  if (hi < 0) return;
  // each event keeps its distance to the others; stop at the first / last track
  delta = std::max(delta, -lo);
  delta = std::min(delta, (int)s.tracks.size() - 1 - hi);
  if (!delta) return;
  for (auto &p : byTrack) st->imp->moveClips({p.second}, p.first + delta, st->moveAutomation);
  st->edited = true;
}

void keepTrackChoices(State *st, const std::function<void()> &rebuild) {
  Session &s = st->imp->session();
  std::vector<std::pair<std::string, std::pair<bool, uint32_t>>> keep;
  for (auto &t : s.tracks) keep.push_back({t.name, {t.import, t.color}});
  rebuild();
  for (auto &t : s.tracks)
    for (auto &k : keep)
      if (k.first == t.name) {
        t.import = k.second.first;
        t.color = k.second.second;
      }
}

void click(State *st, int x, int y, bool shift) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  const Hit *h = hitAt(st, x, y);
  int id = h ? h->id : H_NONE;
  if (st->palTrack >= 0 && id != H_PAL && id != H_PAL_CUSTOM && id != H_PAL_NONE && id != H_TRACK_COLOR) {
    st->palTrack = -1;
    InvalidateRect(st->hwnd, NULL, FALSE);
    if (id == H_LIST || id == H_TIMELINE || id == H_NONE) return;
  }
  switch (id) {
  case H_TIMELINE: {
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
  case H_TRACK_COLOR: st->palTrack = st->palTrack == h->idx ? -1 : h->idx; break;
  case H_PAL: s.tracks[(size_t)st->palTrack].color = kPalette[h->idx]; st->palTrack = -1; break;
  case H_PAL_NONE: s.tracks[(size_t)st->palTrack].color = 0; st->palTrack = -1; break;
  case H_PAL_CUSTOM: {
    uint32_t c = s.tracks[(size_t)st->palTrack].color;
    if (st->host->pickColor && st->host->pickColor(st->hwnd, &c)) s.tracks[(size_t)st->palTrack].color = c ? c : 1;
    st->palTrack = -1;
    break;
  }
  case H_SEL_ALL: for (auto &t : s.tracks) t.import = true; break;
  case H_SEL_NONE: for (auto &t : s.tracks) t.import = false; break;
  case H_ZOOM_FIT: {
    double a, b;
    sessionRange(s, &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
    break;
  }
  case H_ZOOM_IN: st->viewStart += st->viewLen * 0.25; st->viewLen *= 0.5; break;
  case H_ZOOM_OUT: st->viewStart -= st->viewLen * 0.5; st->viewLen *= 2.0; break;
  case H_MERGE: {
    st->merge = !st->merge;
    keepTrackChoices(st, [&]() { st->imp->setMergeStereo(st->merge); });
    opt.mergeStereo = st->merge;
    st->sel.clear();
    st->edited = false;
    break;
  }
  case H_FADES: opt.fades = !opt.fades; break;
  case H_GAIN: opt.clipGain = !opt.clipGain; break;
  case H_VIDEO: opt.video = !opt.video; break;
  case H_MARKERS: opt.markers = !opt.markers; break;
  case H_FOLDERS: opt.folders = !opt.folders; break;
  case H_THEME: setDarkTheme(!darkTheme()); break;
  case H_COPY_YES: opt.copyToSubdir = true; break;
  case H_COPY_NO: opt.copyToSubdir = false; break;
  case H_SUBDIR: {
    std::string v = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
    if (opt.copyToSubdir && st->host->askText && st->host->askText("olAAF", "Subfolder name", &v) && !v.empty())
      opt.subdir = safeFileName(v);
    break;
  }
  case H_DEST: {
    std::string v = opt.destDir;
    if (st->host->askText && st->host->askText("olAAF", "Destination folder", &v) && !v.empty()) opt.destDir = v;
    break;
  }
  case H_DEST_CHOOSE: {
    std::string p;
    if (pickFolder(st->hwnd, "olAAF: choose the destination folder", opt.destDir, &p)) opt.destDir = p;
    break;
  }
  case H_LOCATE: {
    std::string p;
    if (pickFolder(st->hwnd, "olAAF: choose the folder that holds the media", dirName(s.aafPath), &p)) {
      opt.searchFolders.push_back(p);
      st->imp->locateMedia(opt);
    }
    break;
  }
  case H_TC_ON: st->tcOn = !st->tcOn; break;
  case H_TC_PLUS: st->tcMinus = false; st->tcOn = true; break;
  case H_TC_MINUS: st->tcMinus = true; st->tcOn = true; break;
  case H_TC_VALUE: {
    std::string v = formatTC(st->tcAbs, s.fps);
    double sec;
    if (st->host->askText && st->host->askText("olAAF", "Timecode offset (HH:MM:SS:FF)", &v) && parseTC(v, s.fps, &sec)) {
      st->tcAbs = std::fabs(sec);
      if (sec < 0) st->tcMinus = true;
      st->tcOn = true;
    }
    break;
  }
  case H_TC_CURSOR: {
    if (st->host->editCursor) {
      double d = st->host->editCursor() - s.firstClip();
      st->tcAbs = std::fabs(d);
      st->tcMinus = d < 0;
      st->tcOn = true;
    }
    break;
  }
  case H_EV_REMOVE: st->imp->setExcluded(selection(st), true); st->edited = true; break;
  case H_EV_RESTORE: st->imp->setExcluded(selection(st), false); st->edited = true; break;
  case H_EV_UP: moveSelectionBy(st, -1); break;
  case H_EV_DOWN: moveSelectionBy(st, 1); break;
  case H_EV_AUTOMATION: st->moveAutomation = !st->moveAutomation; break;
  case H_EV_RESET:
    keepTrackChoices(st, [&]() { st->imp->setMergeStereo(st->merge); });
    st->sel.clear();
    st->edited = false;
    break;
  case H_CANCEL: EndDialog(st->hwnd, 0); return;
  case H_IMPORT:
    opt.tcOffset = tcOffset(st);
    st->accepted = true;
    EndDialog(st->hwnd, 1);
    return;
  default: break;
  }
  InvalidateRect(st->hwnd, NULL, FALSE);
}

bool shiftDown() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

INT_PTR CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  State *st = (State *)OL_GETUSER(hwnd);
  switch (msg) {
  case WM_INITDIALOG: {
    st = (State *)lp;
    OL_SETUSER(hwnd, st);
    st->hwnd = hwnd;
#ifdef _WIN32
    HDC dc = GetDC(hwnd);
    st->scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(hwnd, dc);
    RECT want = {0, 0, (LONG)std::lround(W * st->scale), (LONG)std::lround(H * st->scale)};
    AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT pr;
    HWND par = GetParent(hwnd);
    if (par && GetWindowRect(par, &pr))
      SetWindowPos(hwnd, NULL, std::max((int)pr.left, (int)(pr.left + pr.right - w) / 2),
                   std::max((int)pr.top, (int)(pr.top + pr.bottom - h) / 2), w, h, SWP_NOZORDER);
#endif
    SetWindowText(hwnd, "olAAF - AAF Import for Reaper");
    makeFonts(st);
    double a, b;
    sessionRange(st->imp->session(), &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
    st->tcAbs = std::floor(st->imp->session().firstClip() / 3600.0) * 3600.0;
    return 0;
  }
  case WM_ERASEBKGND: return 1;
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
    st->dragMoved = false;
    st->pressedClip = 0;
    if (PtInRect(&st->vTrack, p) && st->rowsTotal > kRows) {
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
    } else if (st->palTrack < 0) {
      if (const ClipHit *c = clipAt(st, p.x, p.y)) {
        // pressing an unselected event selects it, so it can be dragged right away
        if (!st->sel.count(c->id) && !shiftDown()) {
          st->sel.clear();
          st->sel.insert(c->id);
        } else
          st->pressedClip = c->id; // shift-click or click on a selected event: decided on release
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
      int range = st->rowsTotal - kRows;
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
    if (st->drag == 3) {
      int dy = my - st->dragFromY;
      if (std::abs(dy) > (int)std::lround(5 * st->scale)) st->dragMoved = true;
      if (st->dragMoved) {
        st->dragRows = (int)std::lround(dy / (kRowH * st->scale));
        st->tip = "Release to move the selected events to this track.";
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
    if (shiftDown()) st->viewStart -= st->viewLen * 0.1 * (delta > 0 ? 1 : -1);
    else st->scrollRow += delta > 0 ? -1 : 1;
    if (st->scrollRow < 0) st->scrollRow = 0;
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == IDCANCEL) EndDialog(hwnd, 0);
    return 0;
  case WM_CLOSE:
    EndDialog(hwnd, 0);
    return 0;
  case WM_DESTROY:
    if (st) {
      HFONT fs[] = {st->fTheme, st->fCheck, st->fClip, st->fTitle, st->fTitleThin, st->fTag, st->fBody, st->fBold, st->fSmall, st->fSec, st->fLogo};
      for (HFONT f : fs)
        if (f) DeleteObject(f);
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
void dialogPaintTest(Importer &imp, ImportOptions &opt, HDC dc, int palTrack, const char *tip) {
  static DialogHost host;
  State st;
  st.imp = &imp;
  st.opt = &opt;
  st.host = &host;
  st.hwnd = (HWND)1;
  st.merge = opt.mergeStereo;
  st.palTrack = palTrack;
  st.tip = tip;
  makeFonts(&st);
  double a, b;
  sessionRange(imp.session(), &a, &b);
  st.viewStart = a - (b - a) * 0.02;
  st.viewLen = (b - a) * 1.04;
  st.tcOn = true;
  st.tcAbs = std::floor(imp.session().firstClip() / 3600.0) * 3600.0;
  // select an event of the first track and remove another, to show the event panel
  auto &tr = imp.session().tracks;
  if (!tr.empty() && tr[0].clips.size() > 2) {
    st.sel.insert(tr[0].clips[2].id);
    imp.setExcluded({tr[0].clips[1].id}, true);
    st.edited = true;
  }
  paint(&st, dc);
}
#endif

bool showImportDialog(HWND parent, Importer &imp, ImportOptions &opt, const DialogHost &host) {
  State st;
  st.imp = &imp;
  st.opt = &opt;
  st.host = &host;
  st.merge = opt.mergeStereo;
#ifdef _WIN32
  DlgTemplate dt;
  memset(&dt, 0, sizeof(dt));
  dt.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
  dt.t.cx = 600;
  dt.t.cy = 400;
  wcscpy(dt.title, L"olAAF");
  DialogBoxIndirectParam((HINSTANCE)g_hInst, &dt.t, parent, (DLGPROC)dlgProc, (LPARAM)&st);
#else
  DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_OLAAF_IMPORT), parent, dlgProc, (LPARAM)&st);
#endif
  return st.accepted;
}

} // namespace olaaf
