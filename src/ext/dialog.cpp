// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The import dialog is drawn by hand (like the other dmajorsound plugins), so it looks the same
// on macOS and Windows: light mode, square controls, explanations bottom left.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "dialog.h"
#include "../core/util.h"

#ifndef _WIN32
#include "../../third_party/reaper-sdk/WDL/swell/swell-dlggen.h"
#ifndef SWELL_DLG_WS_DEFAULT_SCALING
#define SWELL_DLG_WS_DEFAULT_SCALING 0
#endif
#define IDD_OLAAF_IMPORT 4712
SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_OLAAF_IMPORT, SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE | SWELL_DLG_WS_OPAQUE,
                                   "olAAF", 1180, 760, 1.0)
BEGIN
END
SWELL_DEFINE_DIALOG_RESOURCE_END(IDD_OLAAF_IMPORT)
#define OL_GETUSER(h) GetWindowLong(h, GWL_USERDATA)
#define OL_SETUSER(h, v) SetWindowLong(h, GWL_USERDATA, (LONG_PTR)(v))
#else
#define OL_GETUSER(h) GetWindowLongPtr(h, GWLP_USERDATA)
#define OL_SETUSER(h, v) SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)(v))
#endif
#undef min
#undef max

namespace olaaf {

static void *g_hInst = nullptr;
void dialogSetInstance(void *h) { g_hInst = h; }

namespace {

const int W = 1180, H = 760;

const COLORREF kFace = RGB(0xe9, 0xeb, 0xee);
const COLORREF kPanel = RGB(0xff, 0xff, 0xff);
const COLORREF kLine = RGB(0xc4, 0xc9, 0xd0);
const COLORREF kSoft = RGB(0xec, 0xee, 0xf1);
const COLORREF kMute = RGB(0x66, 0x70, 0x7b);
const COLORREF kInk = RGB(0x2b, 0x30, 0x36);
const COLORREF kBlue = RGB(0x55, 0x80, 0xd4);
const COLORREF kBlueSoft = RGB(0xee, 0xf3, 0xfd);
const COLORREF kRed = RGB(0xb3, 0x26, 0x1e);

const uint32_t kPalette[10] = {0x5580d4, 0x1F9A8A, 0x4E8F3A, 0xB8962E, 0xD9822B,
                               0xC8423B, 0xB0457E, 0x7A5AC8, 0x6B7480, 0x2B3440};

COLORREF rgbToRef(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

enum HitId {
  H_NONE, H_TRACK_CHECK, H_TRACK_COLOR, H_SEL_ALL, H_SEL_NONE, H_ZOOM_IN, H_ZOOM_OUT, H_MERGE, H_FADES, H_GAIN,
  H_COPY_YES, H_COPY_NO, H_SUBDIR, H_DEST, H_DEST_CHOOSE, H_LOCATE, H_CANCEL, H_IMPORT, H_TC_ON, H_TC_PLUS,
  H_TC_MINUS, H_TC_VALUE, H_TC_CURSOR, H_PAL, H_PAL_CUSTOM, H_PAL_NONE, H_LIST, H_TIMELINE
};

struct Hit {
  RECT r;
  int id;
  int idx;
  const char *tip;
};

struct State {
  Importer *imp = nullptr;
  ImportOptions *opt = nullptr;
  const DialogHost *host = nullptr;
  HWND hwnd = nullptr;
  double scale = 1.0;
  std::vector<Hit> hits;
  const char *tip = nullptr;
  int scrollRow = 0;
  double viewStart = 0, viewLen = 60; // seconds shown in the preview
  bool tcOn = false, tcMinus = true;
  double tcAbs = 0;   // offset size in seconds
  int palTrack = -1;  // track whose color palette is open
  bool merge = true;
  bool accepted = false;
  HFONT fTitle = nullptr, fTag = nullptr, fBody = nullptr, fBold = nullptr, fSmall = nullptr, fSec = nullptr;
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

void checkbox(State *st, const Gfx &g, double x, double y, bool on, int id, const char *tip) {
  if (on) {
    g.box(x, y, x + 16, y + 16, kBlue, kBlue);
    g.line(x + 3.5, y + 8.5, x + 6.5, y + 11.5, kPanel);
    g.line(x + 6.5, y + 11.5, x + 12.5, y + 4.5, kPanel);
    g.line(x + 3.5, y + 9.5, x + 6.5, y + 12.5, kPanel);
    g.line(x + 6.5, y + 12.5, x + 12.5, y + 5.5, kPanel);
  } else
    g.box(x, y, x + 16, y + 16, RGB(0xa9, 0xaf, 0xb8), kPanel);
  addHit(st, x - 4, y - 4, x + 20, y + 20, id, 0, tip);
}

void button(State *st, const Gfx &g, double l, double t, double r, double b, const std::string &label, int id,
            int idx, const char *tip, bool primary = false) {
  if (primary) {
    g.fill(l, t, r, b, kBlue);
    g.text(st->fBold, kPanel, label, l, t, r, b, DT_CENTER);
  } else {
    g.box(l, t, r, b, kLine, kPanel);
    g.text(st->fBody, kInk, label, l, t, r, b, DT_CENTER);
  }
  addHit(st, l, t, r, b, id, idx, tip);
}

void segmented(State *st, const Gfx &g, double l, double t, double w, double h, const char *a, const char *b,
               bool firstOn, int idA, int idB, const char *tip) {
  g.box(l, t, l + w, t + h, kLine, kPanel);
  double m = l + w / 2;
  if (firstOn) {
    g.fill(l + 2, t + 2, m - 1, t + h - 2, kBlue);
    g.text(st->fBold, kPanel, a, l, t, m, t + h, DT_CENTER);
    g.text(st->fBody, kMute, b, m, t, l + w, t + h, DT_CENTER);
  } else {
    g.fill(m + 1, t + 2, l + w - 2, t + h - 2, kBlue);
    g.text(st->fBody, kMute, a, l, t, m, t + h, DT_CENTER);
    g.text(st->fBold, kPanel, b, m, t, l + w, t + h, DT_CENTER);
  }
  addHit(st, l, t, m, t + h, idA, 0, tip);
  addHit(st, m, t, l + w, t + h, idB, 0, tip);
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

void paint(State *st, HDC dc) {
  Session &s = st->imp->session();
  ImportOptions &opt = *st->opt;
  Gfx g{dc, st->scale};
  st->hits.clear();
  RECT cr;
  GetClientRect(st->hwnd, &cr);
  double cw = cr.right / st->scale, ch = cr.bottom / st->scale;
  g.fill(0, 0, cw, ch, kFace);

  // ---- header
  g.fill(16, 18, 46, 48, kBlue);
  g.fill(23, 25, 33, 28.5, kPanel);
  g.fill(27, 31, 39, 34.5, kPanel);
  g.fill(23, 37, 30, 40.5, kPanel);
  g.text(st->fTitle, kInk, "olAAF", 56, 10, 400, 36, DT_LEFT);
  g.text(st->fTag, kMute, "Do you wanna build a Reaper Session?", 56, 34, 400, 50, DT_LEFT);

  // ---- file bar
  double fy = 64, fb = 112;
  g.box(16, fy, cw - 16, fb, kLine, kPanel);
  g.text(st->fBold, kInk, baseName(s.aafPath), 30, fy + 6, 520, fy + 26, DT_LEFT);
  std::string meta = strprintf("%s  \xc2\xb7  %d Hz \xc2\xb7 %d bit \xc2\xb7 %g fps \xc2\xb7 Start %s \xc2\xb7 %zu tracks",
                               s.product.c_str(), s.sampleRate, s.bits, s.fps, formatTC(s.start, s.fps).c_str(),
                               s.tracks.size());
  g.text(st->fSmall, kMute, meta, 30, fy + 26, 560, fy + 42, DT_LEFT);
  // timecode offset group (right aligned)
  double x = cw - 16 - 610;
  checkbox(st, g, x, fy + 16, st->tcOn, H_TC_ON, "Timecode offset: shifts every clip by this amount on import.");
  g.text(st->fBody, kInk, "Timecode offset", x + 24, fy + 10, x + 140, fy + 38, DT_LEFT);
  segmented(st, g, x + 140, fy + 10, 56, 28, "+", "\xe2\x88\x92", !st->tcMinus, H_TC_PLUS, H_TC_MINUS,
            "Add or subtract the offset.");
  g.box(x + 204, fy + 10, x + 304, fy + 38, kLine, kPanel);
  g.text(st->fBody, kInk, formatTC(st->tcAbs, s.fps), x + 204, fy + 10, x + 304, fy + 38, DT_CENTER);
  addHit(st, x + 204, fy + 10, x + 304, fy + 38, H_TC_VALUE, 0, "Click to type the offset as HH:MM:SS:FF.");
  double first = s.firstClip();
  g.text(st->fSmall, kMute, "\xe2\x86\x92 Starts at", x + 312, fy + 10, x + 380, fy + 38, DT_LEFT);
  g.text(st->fBold, kInk, formatTC(first + tcOffset(st), s.fps), x + 380, fy + 10, x + 476, fy + 38, DT_LEFT);
  button(st, g, x + 484, fy + 10, x + 594, fy + 38, "Edit cursor", H_TC_CURSOR, 0,
         "Sets the offset so the AAF starts at the REAPER edit cursor.");

  // ---- right column: options
  double ax = cw - 16 - 320, ar = cw - 16, ay = 124;
  // Clips
  g.box(ax, ay, ar, ay + 128, kLine, kPanel);
  g.text(st->fSec, kMute, "CLIPS", ax + 14, ay + 8, ar, ay + 26, DT_LEFT);
  struct Opt { const char *label; bool on; int id; const char *tip; } opts[3] = {
      {"Merge .L / .R to stereo", st->merge, H_MERGE, "Two mono tracks named .L and .R whose clips line up become one stereo track."},
      {"Import fades and crossfades", opt.fades, H_FADES, "Fade ins, fade outs and crossfades from the AAF."},
      {"Import clip gain", opt.clipGain, H_GAIN, "Clip gain becomes item volume, clip gain automation a take volume envelope."}};
  for (int i = 0; i < 3; i++) {
    double yy = ay + 34 + i * 30;
    g.text(st->fBody, kInk, opts[i].label, ax + 14, yy, ar - 40, yy + 24, DT_LEFT);
    checkbox(st, g, ar - 30, yy + 4, opts[i].on, opts[i].id, opts[i].tip);
  }
  // Media
  double my = ay + 140;
  g.box(ax, my, ar, my + 250, kLine, kPanel);
  g.text(st->fSec, kMute, "MEDIA", ax + 14, my + 8, ar, my + 26, DT_LEFT);
  g.text(st->fBody, kInk, "Copy audio files into subdirectory", ax + 14, my + 30, ar - 14, my + 52, DT_LEFT);
  segmented(st, g, ax + 14, my + 54, ar - ax - 28, 28, "Yes", "No", opt.copyToSubdir, H_COPY_YES, H_COPY_NO,
            "Yes: WAV files are copied into the subfolder. No: they stay where they are.");
  g.text(st->fSmall, kMute, "Subdirectory name", ax + 14, my + 90, ar, my + 106, DT_LEFT);
  std::string sub = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
  g.box(ax + 14, my + 108, ar - 14, my + 136, kLine, opt.copyToSubdir ? kPanel : kSoft);
  g.text(st->fBody, opt.copyToSubdir ? kInk : kMute, sub, ax + 22, my + 108, ar - 22, my + 136, DT_LEFT);
  addHit(st, ax + 14, my + 108, ar - 14, my + 136, H_SUBDIR, 0, "Click to rename the audio folder.");
  g.text(st->fSmall, kMute, "Destination", ax + 14, my + 142, ar, my + 158, DT_LEFT);
  g.box(ax + 14, my + 160, ar - 104, my + 188, kLine, kPanel);
  g.text(st->fBody, kInk, opt.destDir, ax + 22, my + 160, ar - 110, my + 188, DT_LEFT);
  addHit(st, ax + 14, my + 160, ar - 104, my + 188, H_DEST, 0, "Click to type a destination folder.");
  button(st, g, ar - 96, my + 160, ar - 14, my + 188, "Choose\xe2\x80\xa6", H_DEST_CHOOSE, 0,
         "Pick any file inside the folder you want to use.");
  g.text(st->fSmall, kMute, "WAV stays WAV. MXF, AIFF and embedded audio", ax + 14, my + 196, ar - 14, my + 214, DT_LEFT);
  g.text(st->fSmall, kMute, "become Broadcast WAV with timecode.", ax + 14, my + 212, ar - 14, my + 230, DT_LEFT);
  // Status
  int total = 0, ok = 0;
  std::set<int> used;
  for (auto &t : s.tracks)
    if (t.import)
      for (auto &c : t.clips)
        for (auto &src : c.sources)
          if (src.media >= 0) used.insert(src.media);
  for (int mi : used) {
    total++;
    const Media &m = s.media[(size_t)mi];
    if (m.kind == MediaKind::Embedded || fileExists(m.foundPath)) ok++;
  }
  double sy = my + 262;
  g.box(ax, sy, ar, sy + 92, kLine, kPanel);
  g.text(st->fSec, kMute, "STATUS", ax + 14, sy + 8, ar, sy + 26, DT_LEFT);
  g.text(st->fBody, kInk, "Media found", ax + 14, sy + 30, ar - 14, sy + 52, DT_LEFT);
  g.text(st->fBody, kInk, strprintf("%d / %d", ok, total), ax + 14, sy + 30, ar - 14, sy + 52, DT_RIGHT);
  if (ok < total) {
    g.text(st->fBody, kRed, strprintf("%d files missing", total - ok), ax + 14, sy + 56, ar - 150, sy + 80, DT_LEFT);
    g.text(st->fBody, kBlue, "Locate media folder\xe2\x80\xa6", ax + 14, sy + 56, ar - 14, sy + 80, DT_RIGHT);
    addHit(st, ar - 170, sy + 56, ar - 14, sy + 80, H_LOCATE, 0, "Pick any audio file in the folder that holds the media.");
  } else
    g.text(st->fBody, RGB(0x1D, 0x9E, 0x75), "All media is there.", ax + 14, sy + 56, ar - 14, sy + 80, DT_LEFT);

  // buttons
  int nsel = 0;
  for (auto &t : s.tracks) nsel += t.import;
  double by = ch - 92;
  button(st, g, ar - 300, by, ar - 190, by + 36, "Cancel", H_CANCEL, 0, "Close without importing.");
  button(st, g, ar - 180, by, ar, by + 36, strprintf("Import %d track%s", nsel, nsel == 1 ? "" : "s"), H_IMPORT, 0,
         "Import the selected tracks into REAPER.", true);

  // ---- tracks + preview
  double lx = 16, lr = ax - 12, ly = 124, lb = ch - 100;
  g.box(lx, ly, lr, lb, kLine, kPanel);
  g.fill(lx + 1, ly + 40, lr - 1, ly + 41, kLine);
  g.text(st->fSec, kMute, "TRACKS", lx + 12, ly, lx + 80, ly + 40, DT_LEFT);
  g.text(st->fSmall, kMute, strprintf("%d of %zu selected", nsel, s.tracks.size()), lx + 80, ly, lx + 220, ly + 40, DT_LEFT);
  button(st, g, lx + 220, ly + 7, lx + 300, ly + 33, "Select all", H_SEL_ALL, 0, "Select every track.");
  button(st, g, lx + 306, ly + 7, lx + 360, ly + 33, "None", H_SEL_NONE, 0, "Deselect every track.");
  g.fill(lx + 372, ly + 12, lx + 373, ly + 28, kLine);
  g.text(st->fSec, kMute, "PREVIEW", lx + 384, ly, lx + 470, ly + 40, DT_LEFT);
  button(st, g, lr - 70, ly + 7, lr - 42, ly + 33, "\xe2\x88\x92", H_ZOOM_OUT, 0, "Zoom out. Shift + mouse wheel scrolls the timeline.");
  button(st, g, lr - 36, ly + 7, lr - 8, ly + 33, "+", H_ZOOM_IN, 0, "Zoom in. Shift + mouse wheel scrolls the timeline.");

  const double rowH = 34, listW = 300, top = ly + 41 + 26;
  int rows = std::max(1, (int)((lb - top - 1) / rowH));
  int ntr = (int)s.tracks.size();
  st->scrollRow = std::max(0, std::min(st->scrollRow, std::max(0, ntr - rows)));
  // list header and ruler
  g.fill(lx + 1, ly + 41, lr - 1, top, RGB(0xfa, 0xfb, 0xfc));
  g.fill(lx + 1, top, lr - 1, top + 1, kLine);
  g.fill(lx + listW, ly + 41, lx + listW + 1, lb - 1, kLine);
  g.text(st->fSmall, kMute, "Name", lx + 60, ly + 41, lx + 200, top, DT_LEFT);
  g.text(st->fSmall, kMute, "Clips", lx + 200, ly + 41, lx + listW - 12, top, DT_RIGHT);
  addHit(st, lx, top, lx + listW, lb, H_LIST, 0, "Click the square to set a track color. Mouse wheel scrolls.");

  double tlx = lx + listW + 10, tlr = lr - 10, tlw = tlr - tlx;
  double spp = st->viewLen / tlw; // seconds per pixel
  double step = niceStep(spp);
  double off = tcOffset(st);
  double t0 = std::floor((st->viewStart + off) / step) * step;
  for (double tt = t0; tt <= st->viewStart + off + st->viewLen; tt += step) {
    double px = tlx + (tt - off - st->viewStart) / spp;
    if (px < tlx || px > tlr) continue;
    g.fill(px, ly + 41, px + 1, lb - 1, kSoft);
    g.fill(px, ly + 41, px + 1, top, kLine);
    g.text(st->fSmall, kMute, formatTC(tt, s.fps), px + 4, ly + 41, std::min(px + 100, tlr), top, DT_LEFT);
  }
  addHit(st, tlx, top, tlr, lb, H_TIMELINE, 0, "Mouse wheel scrolls the tracks, Shift + wheel moves through time.");

  for (int r = 0; r < rows && st->scrollRow + r < ntr; r++) {
    int ti = st->scrollRow + r;
    Track &t = s.tracks[(size_t)ti];
    double y = top + 1 + r * rowH;
    bool on = t.import;
    COLORREF tc = t.color ? rgbToRef(t.color) : RGB(0x9a, 0xa2, 0xad);
    g.fill(lx + 1, y + rowH - 1, lr - 1, y + rowH, kSoft);
    checkbox(st, g, lx + 12, y + 9, on, H_TRACK_CHECK, "Import this track or leave it out.");
    st->hits.back().idx = ti;
    if (t.color) g.box(lx + 36, y + 9, lx + 52, y + 25, RGB(0x99, 0x99, 0x99), tc);
    else {
      g.box(lx + 36, y + 9, lx + 52, y + 25, RGB(0xa9, 0xaf, 0xb8), kPanel);
      g.line(lx + 38, y + 23, lx + 50, y + 11, RGB(0xa9, 0xaf, 0xb8));
    }
    addHit(st, lx + 34, y + 7, lx + 54, y + 27, H_TRACK_COLOR, ti, "Set the track color.");
    COLORREF nameC = on ? kInk : kMute;
    if (!t.mergedFrom.empty()) {
      g.text(st->fBody, nameC, t.name, lx + 60, y + 2, lx + listW - 90, y + 20, DT_LEFT);
      g.text(st->fSmall, kMute, t.mergedFrom, lx + 60, y + 18, lx + listW - 50, y + 32, DT_LEFT);
    } else
      g.text(st->fBody, nameC, t.name, lx + 60, y, lx + listW - 90, y + rowH, DT_LEFT);
    if (t.channels >= 2) {
      const char *badge = t.channels == 2 ? (t.mergedFrom.empty() ? "ST" : "L+R") : "MC";
      g.box(lx + listW - 84, y + 10, lx + listW - 52, y + 24, RGB(0xb9, 0xcc, 0xf2), kBlueSoft);
      g.text(st->fSmall, kBlue, badge, lx + listW - 84, y + 10, lx + listW - 52, y + 24, DT_CENTER);
    }
    g.text(st->fSmall, kMute, std::to_string(t.clips.size()), lx + 200, y, lx + listW - 12, y + rowH, DT_RIGHT);

    // clips
    COLORREF cc = t.color ? tc : RGB(0x7f, 0x9c, 0xd8);
    if (!on) cc = RGB(0xd3, 0xd7, 0xdc);
    for (auto &c : t.clips) {
      double a = tlx + (c.pos - st->viewStart) / spp, b = tlx + (c.pos + c.len - st->viewStart) / spp;
      if (b < tlx || a > tlr) continue;
      a = std::max(a, tlx);
      b = std::min(b, tlr);
      if (b - a < 1) b = a + 1;
      g.fill(a, y + 5, b, y + rowH - 6, cc);
      double fi = c.fadeIn.len / spp, fo = c.fadeOut.len / spp;
      if (opt.fades && fi > 2) g.line(a, y + rowH - 6, std::min(b, a + fi), y + 5, kInk);
      if (opt.fades && fo > 2) g.line(std::max(a, b - fo), y + 5, b, y + rowH - 6, kInk);
    }
  }
  if (ntr > rows) {
    double sh = (lb - top) * rows / ntr, sp = top + (lb - top - sh) * st->scrollRow / std::max(1, ntr - rows);
    g.fill(lr - 5, sp, lr - 2, sp + sh, kLine);
  }

  // ---- color palette popover
  if (st->palTrack >= 0 && st->palTrack >= st->scrollRow && st->palTrack < st->scrollRow + rows) {
    double py = top + 1 + (st->palTrack - st->scrollRow + 1) * rowH;
    if (py + 130 > lb) py -= rowH + 130;
    double px = lx + 30;
    g.box(px, py, px + 176, py + 126, kLine, kPanel);
    g.text(st->fSec, kMute, "COLOR \xc2\xb7 " + s.tracks[(size_t)st->palTrack].name, px + 10, py + 4, px + 170, py + 24, DT_LEFT);
    for (int i = 0; i < 10; i++) {
      double sx = px + 10 + (i % 5) * 32, sy2 = py + 28 + (i / 5) * 30;
      bool cur = s.tracks[(size_t)st->palTrack].color == kPalette[i];
      g.box(sx, sy2, sx + 24, sy2 + 24, cur ? kInk : RGB(0x99, 0x99, 0x99), rgbToRef(kPalette[i]));
      addHit(st, sx, sy2, sx + 24, sy2 + 24, H_PAL, i, "Pick this color.");
    }
    button(st, g, px + 10, py + 92, px + 86, py + 116, "Custom\xe2\x80\xa6", H_PAL_CUSTOM, 0, "Any color.");
    button(st, g, px + 92, py + 92, px + 166, py + 116, "None", H_PAL_NONE, 0, "REAPER's default color.");
  }

  // ---- footer: explanation above the line, version below
  g.text(st->fSmall, kMute,
         st->tip ? st->tip : "Pick the tracks to import, check the options, then Import.", 16, ch - 46, cw - 16, ch - 28,
         DT_LEFT);
  g.fill(16, ch - 26, cw - 16, ch - 25, kLine);
  g.text(st->fTag, kMute, "V 0.3 beta developed with AI assistance by dmajorsound", 16, ch - 22, cw - 16, ch - 6, DT_LEFT);
}

const Hit *hitAt(State *st, int x, int y) {
  // last drawn wins (popover is on top)
  for (auto it = st->hits.rbegin(); it != st->hits.rend(); ++it) {
    POINT p = {x, y};
    if (PtInRect(&it->r, p)) return &*it;
  }
  return nullptr;
}

void click(State *st, int x, int y) {
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
  case H_ZOOM_IN: st->viewStart += st->viewLen * 0.25; st->viewLen *= 0.5; break;
  case H_ZOOM_OUT: st->viewStart -= st->viewLen * 0.5; st->viewLen *= 2.0; break;
  case H_MERGE: {
    st->merge = !st->merge;
    // keep track choices by name across the merge switch
    std::vector<std::pair<std::string, std::pair<bool, uint32_t>>> keep;
    for (auto &t : s.tracks) keep.push_back({t.name, {t.import, t.color}});
    st->imp->setMergeStereo(st->merge);
    for (auto &t : s.tracks)
      for (auto &k : keep)
        if (k.first == t.name) { t.import = k.second.first; t.color = k.second.second; }
    opt.mergeStereo = st->merge;
    break;
  }
  case H_FADES: opt.fades = !opt.fades; break;
  case H_GAIN: opt.clipGain = !opt.clipGain; break;
  case H_COPY_YES: opt.copyToSubdir = true; break;
  case H_COPY_NO: opt.copyToSubdir = false; break;
  case H_SUBDIR: {
    std::string v = opt.subdir.empty() ? defaultSubdir(s) : opt.subdir;
    if (opt.copyToSubdir && st->host->askText && st->host->askText("olAAF", "Subdirectory name", &v) && !v.empty())
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
    if (st->host->pickFile && st->host->pickFile("olAAF: pick any file in the destination folder", &p) && !p.empty())
      opt.destDir = dirName(p);
    break;
  }
  case H_LOCATE: {
    std::string p;
    if (st->host->pickFile && st->host->pickFile("olAAF: pick any audio file in the media folder", &p) && !p.empty()) {
      opt.searchFolders.push_back(dirName(p));
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
  case H_CANCEL: EndDialog(st->hwnd, 0); return;
  case H_IMPORT: {
    int n = 0;
    for (auto &t : s.tracks) n += t.import;
    if (!n) return;
    opt.tcOffset = tcOffset(st);
    st->accepted = true;
    EndDialog(st->hwnd, 1);
    return;
  }
  default: break;
  }
  InvalidateRect(st->hwnd, NULL, FALSE);
}

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
    SetWindowText(hwnd, "olAAF - Import AAF");
    st->fTitle = makeFont(19, true, st->scale);
    st->fTag = makeFont(10.5, false, st->scale);
    st->fBody = makeFont(13, false, st->scale);
    st->fBold = makeFont(13, true, st->scale);
    st->fSmall = makeFont(11.5, false, st->scale);
    st->fSec = makeFont(10.5, true, st->scale);
    double a, b;
    sessionRange(st->imp->session(), &a, &b);
    st->viewStart = a - (b - a) * 0.02;
    st->viewLen = (b - a) * 1.04;
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
  case WM_LBUTTONUP:
    click(st, (short)LOWORD(lp), (short)HIWORD(lp));
    return 0;
  case WM_MOUSEMOVE: {
    const Hit *h = hitAt(st, (short)LOWORD(lp), (short)HIWORD(lp));
    const char *tip = h ? h->tip : nullptr;
    if (tip != st->tip) {
      st->tip = tip;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  }
  case WM_MOUSEWHEEL: {
    int delta = (short)HIWORD(wp);
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift)
      st->viewStart -= st->viewLen * 0.1 * (delta > 0 ? 1 : -1);
    else
      st->scrollRow += delta > 0 ? -1 : 1;
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
      DeleteObject(st->fTitle); DeleteObject(st->fTag); DeleteObject(st->fBody);
      DeleteObject(st->fBold); DeleteObject(st->fSmall); DeleteObject(st->fSec);
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
  st.fTitle = makeFont(19, true, 1); st.fTag = makeFont(10.5, false, 1); st.fBody = makeFont(13, false, 1);
  st.fBold = makeFont(13, true, 1); st.fSmall = makeFont(11.5, false, 1); st.fSec = makeFont(10.5, true, 1);
  double a, b;
  sessionRange(imp.session(), &a, &b);
  st.viewStart = a - (b - a) * 0.02;
  st.viewLen = (b - a) * 1.04;
  st.tcOn = true;
  st.tcAbs = imp.session().firstClip();
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
