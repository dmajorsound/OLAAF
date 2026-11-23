// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Colors shared by the dialog and the progress window, taken from the REAPER theme.

#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include "../../third_party/reaper-sdk/WDL/swell/swell.h"
#endif
#undef min
#undef max

#include <string>

namespace olaaf {

// Text in olAAF is UTF-8. Windows needs it as UTF-16 (the plain DrawText / SetWindowText take the
// ANSI code page and turn "–", "·", "▾" into garbage); macOS / Linux (SWELL) take UTF-8 directly.
#ifdef _WIN32
inline std::wstring olWide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
  if (!w.empty() && w.back() == L'\0') w.pop_back();
  return w;
}
inline int olDrawText(HDC dc, const char *s, RECT *rc, UINT fmt) {
  std::wstring w = olWide(s);
  return DrawTextW(dc, w.c_str(), (int)w.size(), rc, fmt);
}
inline void olSetText(HWND h, const char *s) { SetWindowTextW(h, olWide(s).c_str()); }
inline std::string olGetText(HWND h) {
  wchar_t buf[1024] = {0};
  GetWindowTextW(h, buf, 1024);
  char out[4096] = {0};
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL);
  return out;
}
#else
inline int olDrawText(HDC dc, const char *s, RECT *rc, UINT fmt) { return DrawText(dc, s, -1, rc, fmt); }
inline void olSetText(HWND h, const char *s) { SetWindowText(h, s); }
inline std::string olGetText(HWND h) {
  char buf[4096] = {0};
  GetWindowText(h, buf, sizeof(buf));
  return buf;
}
#endif

struct Theme {
  COLORREF face, panel, line, soft, track, mute, ink, blue, blueSoft, blueLine, red, green, thumb, empty, off;
};

// The colors follow REAPER's current theme: get(key) returns REAPER's color for a theme key
// (GetThemeColor) or -1. Missing keys fall back to REAPER 7's default theme.
void setThemeFromReaper(int (*get)(const char *key));
// tests: false = a light theme, true = REAPER 7's default theme
void setDarkTheme(bool dark);
bool darkTheme(); // dark background (text and tints follow it)
const Theme &theme();

} // namespace olaaf
