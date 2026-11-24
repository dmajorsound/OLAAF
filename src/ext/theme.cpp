// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "theme.h"

#include <cmath>

namespace olaaf {

static COLORREF mixC(COLORREF a, COLORREF b, double t) { // t = share of b
  auto m = [&](int x, int y) { return (int)std::lround(x + (y - x) * t); };
  return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}
static double lum(COLORREF c) { return (0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c)) / 255.0; }

static Theme g_theme;
static bool g_dark = true;
static bool g_set = false; // a theme was chosen
static int (*g_reaper)(const char *) = nullptr; // where REAPER's colors come from
static int g_mode = 0; // 0 REAPER's colors (system), 1 dark, 2 light

// Everything else is derived from four colors: window background, lane background, ruler / header
// background and text. One blue marks what is chosen.
static void build(COLORREF face, COLORREF panel, COLORREF track, COLORREF ink) {
  g_set = true;
  g_dark = lum(face) < 0.5;
  // text must stand out from the panel; themes with odd text colors get plain light / dark text
  if (std::fabs(lum(ink) - lum(panel)) < 0.35) ink = g_dark ? RGB(0xdc, 0xdc, 0xdc) : RGB(0x1f, 0x22, 0x26);
  Theme &t = g_theme;
  t.face = face;
  t.panel = panel;
  t.track = track;
  t.ink = ink;
  t.line = g_dark ? mixC(face, RGB(0, 0, 0), 0.45) : mixC(face, RGB(0, 0, 0), 0.12);
  t.soft = mixC(panel, ink, 0.07);
  t.mute = mixC(ink, panel, 0.38);
  t.blue = g_dark ? RGB(0x6b, 0xa4, 0xe0) : RGB(0x4c, 0x93, 0xdc);
  t.blueSoft = mixC(t.blue, panel, 0.80);
  t.blueLine = mixC(t.blue, panel, 0.45);
  t.red = g_dark ? RGB(0xff, 0x7a, 0x6e) : RGB(0xc8, 0x42, 0x3b);
  t.green = g_dark ? RGB(0x80, 0xe2, 0xac) : RGB(0x2e, 0x8b, 0x57);
  t.thumb = mixC(panel, ink, 0.30);
  t.empty = mixC(panel, ink, 0.45);
  t.off = mixC(panel, ink, 0.22);
}

// REAPER 7 default theme (approximate) and a plain light theme for tests
static void buildDefaultDark() { build(RGB(0x33, 0x33, 0x33), RGB(0x2a, 0x2a, 0x2a), RGB(0x3b, 0x3b, 0x3b), RGB(0xdc, 0xdc, 0xdc)); }
static void buildDefaultLight() { build(RGB(0xff, 0xff, 0xff), RGB(0xff, 0xff, 0xff), RGB(0xf5, 0xf5, 0xf6), RGB(0x1f, 0x22, 0x26)); }

void setThemeFromReaper(int (*get)(const char *key)) {
  g_reaper = get;
  if (g_mode == 2) {
    buildDefaultLight();
    return;
  }
  buildDefaultDark();
  if (g_mode == 1) return; // OLAAF's own dark
  if (!get) return;
  auto c = [&](const char *key, COLORREF def) {
    int v = get(key);
    return v < 0 ? def : (COLORREF)v;
  };
  COLORREF face = c("col_main_bg2", g_theme.face);
  COLORREF panel = c("col_arrangebg", g_theme.panel);
  COLORREF track = c("col_tl_bg", g_theme.track);
  COLORREF ink = c("col_main_text2", g_theme.ink);
  build(face, panel, track, ink);
}

void setThemeMode(int mode) {
  g_mode = mode < 0 || mode > 2 ? 0 : mode;
  setThemeFromReaper(g_reaper);
}
int themeMode() { return g_mode; }

void setDarkTheme(bool dark) {
  if (dark) buildDefaultDark();
  else buildDefaultLight();
}
bool darkTheme() { return g_dark; }
const Theme &theme() {
  if (!g_set) buildDefaultDark();
  return g_theme;
}

} // namespace olaaf
