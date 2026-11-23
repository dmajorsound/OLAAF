// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

// C++ headers before SWELL / windows.h (they define min and max)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "progress.h"
#include "theme.h"
#include "logo.h"
#include "logo.h"

#ifndef _WIN32
#include "../../third_party/reaper-sdk/WDL/swell/swell-dlggen.h"
#ifndef SWELL_DLG_WS_DEFAULT_SCALING
#define SWELL_DLG_WS_DEFAULT_SCALING 0
#endif
#define IDD_OLAAF_PROGRESS 4711
SWELL_DEFINE_DIALOG_RESOURCE_BEGIN(IDD_OLAAF_PROGRESS, SWELL_DLG_WS_FLIPPED | SWELL_DLG_WS_NOAUTOSIZE | SWELL_DLG_WS_OPAQUE,
                                   "OLAAF", 560, 340, 1.0)
BEGIN
END
SWELL_DEFINE_DIALOG_RESOURCE_END(IDD_OLAAF_PROGRESS)
#endif

#undef min
#undef max

#ifdef _WIN32
#define OL_GETUSER(h) GetWindowLongPtr(h, GWLP_USERDATA)
#define OL_SETUSER(h, v) SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)(v))
#else
#define OL_GETUSER(h) GetWindowLong(h, GWL_USERDATA)
#define OL_SETUSER(h, v) SetWindowLong(h, GWL_USERDATA, (LONG_PTR)(v))
#endif

namespace olaaf {

static void *g_hInst = nullptr;
void progressSetInstance(void *h) { g_hInst = h; }

namespace {

// look of the other dmajorsound plugins (light mode)
#define kFace (theme().face)
#define kPanel (theme().panel)
#define kLine (theme().line)
#define kMute (theme().mute)
#define kInk (theme().ink)
#define kBlue (theme().blue)
const COLORREF kWhite = RGB(0xff, 0xff, 0xff);
const COLORREF kGreen = RGB(0x1D, 0x9E, 0x75);
const COLORREF kAmber = RGB(0xE8, 0xA0, 0x20);
const COLORREF kRed = RGB(0xe2, 0x3b, 0x2e);

struct Shared {
  std::mutex m;
  std::condition_variable cv;
  double fraction = 0.0;
  std::string detail = "Reading the AAF";
  bool done = false;
  bool result = false;
  std::atomic<bool> cancel{false};
};

struct Ui {
  Shared *sh = nullptr;
  std::chrono::steady_clock::time_point t0;
  RECT cancelRc{};
  double scale = 1.0;
  HFONT fTitle = nullptr, fTag = nullptr, fLine = nullptr, fSmall = nullptr, fBtn = nullptr, fLogo = nullptr;
};

// fun lines, picked by what the worker is doing right now
const char *funLine(const std::string &detail, double t) {
  static const char *reading[] = {"Reading the AAF...", "Lining up the timecode...", "Finding every clip..."};
  static const char *extract[] = {"Unpacking the embedded audio...", "Freeing the audio from the AAF..."};
  static const char *convert[] = {"Converting MXF to Broadcast WAV...", "Repacking the audio, bit for bit..."};
  static const char *copy[] = {"Copying the audio...", "Moving the files into place..."};
  static const char *combine[] = {"Teaching the L to meet the R...", "Building stereo files..."};
  static const char *finish[] = {"Counting fades...", "Building your Reaper session..."};
  const char **set = reading;
  int n = 3;
  auto starts = [&](const char *p) { return detail.compare(0, strlen(p), p) == 0; };
  if (starts("Extracting")) { set = extract; n = 2; }
  else if (starts("Converting")) { set = convert; n = 2; }
  else if (starts("Copying") || starts("Linking")) { set = copy; n = 2; }
  else if (starts("Combining")) { set = combine; n = 2; }
  else if (starts("Done") || starts("Building")) { set = finish; n = 2; }
  return set[(int)(t / 2.2) % n];
}

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

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT rc = {l, t, r, b};
  HBRUSH br = CreateSolidBrush(c);
  FillRect(dc, &rc, br);
  DeleteObject(br);
}

void frame(HDC dc, int l, int t, int r, int b, COLORREF line, COLORREF inside) {
  fill(dc, l, t, r, b, line);
  fill(dc, l + 1, t + 1, r - 1, b - 1, inside);
}

void text(HDC dc, HFONT f, COLORREF c, const std::string &s, int l, int t, int r, int b, int align) {
  HGDIOBJ old = SelectObject(dc, f);
  SetTextColor(dc, c);
  SetBkMode(dc, TRANSPARENT);
  RECT rc = {l, t, r, b};
  olDrawText(dc, s.c_str(), &rc, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(dc, old);
}

void paint(HWND hwnd, HDC dc, Ui *ui) {
  double s = ui->scale;
  auto S = [s](double v) { return (int)std::lround(v * s); };
  RECT cr;
  GetClientRect(hwnd, &cr);

  double fraction;
  std::string detail;
  {
    std::lock_guard<std::mutex> lk(ui->sh->m);
    fraction = ui->sh->fraction;
    detail = ui->sh->detail;
  }
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - ui->t0).count();

  fill(dc, 0, 0, cr.right, cr.bottom, kFace);

  // header: logo and name
  fill(dc, S(16), S(18), S(42), S(44), kBlue);
  fill(dc, S(22), S(24), S(31), S(27), kWhite);
  fill(dc, S(26), S(29), S(37), S(32), kWhite);
  fill(dc, S(22), S(34), S(29), S(37), kWhite);
  text(dc, ui->fTitle, kInk, "OLAAF", S(52), S(16), S(400), S(34), DT_LEFT);
  {
    HGDIOBJ old = SelectObject(dc, ui->fTitle);
    RECT m = {0, 0, 4000, 100};
    olDrawText(dc, "OLAAF", &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    int tw = m.right - m.left;
    if (tw < S(20) || tw > S(200)) tw = S(46);
    text(dc, ui->fLine, kMute, "\xe2\x80\x93 AAF Import for Reaper", S(52) + tw + S(6), S(16), S(520), S(34), DT_LEFT);
  }
  text(dc, ui->fTag, kMute, "Do you wanna build a Reaper Session?", S(52), S(33), S(400), S(46), DT_LEFT);
  drawDmajorLogo(dc, cr.right - S(16), S(31), S(26), kInk, kMute, ui->fTag);

  // panel
  int pl = S(16), pt = S(58), pr = cr.right - S(16), pb = S(250);
  frame(dc, pl, pt, pr, pb, kLine, kPanel);

  // meter bars, same motion as the combfilter loader (1 s ease-in-out, staggered)
  static const double delays[7] = {0, -0.85, -0.55, -0.3, -0.7, -0.15, -0.45};
  int barW = S(8), gap = S(5), barH = S(54);
  int total = 7 * barW + 6 * gap;
  int bx = (cr.right - total) / 2, bottom = S(86) + barH;
  for (int i = 0; i < 7; i++) {
    double ph = std::fmod(t - delays[i], 1.0);
    if (ph < 0) ph += 1.0;
    double x = ph < 0.5 ? ph * 2 : (1 - ph) * 2;
    double e = x * x * (3 - 2 * x);
    int h = (int)std::lround(barH * (0.15 + 0.85 * e));
    int l = bx + i * (barW + gap), r = l + barW, top = bottom - h;
    int g = bottom - (int)std::lround(h * 0.62), a = bottom - (int)std::lround(h * 0.84);
    fill(dc, l, g, r, bottom, kGreen);
    fill(dc, l, a, r, g, kAmber);
    fill(dc, l, top, r, a, kRed);
  }

  // line in capitals
  std::string line = funLine(detail, t);
  for (auto &c : line) c = (char)toupper((unsigned char)c);
  text(dc, ui->fLine, kMute, line, pl + S(16), S(154), pr - S(16), S(172), DT_CENTER);

  // progress bar
  int gl = pl + S(24), gr = pr - S(24) - S(48), gt = S(184), gb = S(193);
  frame(dc, gl, gt, gr, gb, kLine, kFace);
  int fillR = gl + 1 + (int)std::lround((gr - gl - 2) * std::max(0.0, std::min(1.0, fraction)));
  if (fillR > gl + 1) fill(dc, gl + 1, gt + 1, fillR, gb - 1, kBlue);
  char pct[16];
  snprintf(pct, sizeof(pct), "%d%%", (int)std::floor(fraction * 100.0 + 0.5));
  text(dc, ui->fSmall, kInk, pct, gr + S(6), gt - S(4), pr - S(24), gb + S(4), DT_RIGHT);

  // what is really happening
  text(dc, ui->fSmall, kMute, detail, pl + S(16), S(204), pr - S(16), S(222), DT_CENTER);

  // Cancel
  ui->cancelRc = {pr - S(84), S(260), pr, S(290)};
  bool cancelling = ui->sh->cancel.load();
  frame(dc, ui->cancelRc.left, ui->cancelRc.top, ui->cancelRc.right, ui->cancelRc.bottom, kLine, kPanel);
  text(dc, ui->fBtn, kInk, cancelling ? "Stopping..." : "Cancel", ui->cancelRc.left, ui->cancelRc.top,
       ui->cancelRc.right, ui->cancelRc.bottom, DT_CENTER);

  // footer
  fill(dc, pl, S(302), pr, S(303), kLine);
  text(dc, ui->fTag, kMute, "V 0.6 beta developed with AI assistance", pl, S(306), pr, S(324), DT_LEFT);

}

INT_PTR CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Ui *ui = (Ui *)OL_GETUSER(hwnd);
  switch (msg) {
  case WM_INITDIALOG: {
    ui = (Ui *)lp;
    OL_SETUSER(hwnd, ui);
#ifdef _WIN32
    HDC dc = GetDC(hwnd);
    ui->scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(hwnd, dc);
    // size the client area to 560 x 340 (scaled), centred on the parent
    RECT want = {0, 0, (LONG)std::lround(560 * ui->scale), (LONG)std::lround(340 * ui->scale)};
    AdjustWindowRectEx(&want, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    int w = want.right - want.left, h = want.bottom - want.top;
    RECT pr;
    HWND par = GetParent(hwnd);
    if (par && GetWindowRect(par, &pr))
      SetWindowPos(hwnd, NULL, (pr.left + pr.right - w) / 2, (pr.top + pr.bottom - h) / 2, w, h, SWP_NOZORDER);
    else
      SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE);
#else
    ui->scale = 1.0;
#endif
    SetWindowText(hwnd, "OLAAF");
    ui->fTitle = makeFont(16, true, ui->scale);
    ui->fTag = makeFont(10, false, ui->scale);
    ui->fLogo = makeFont(10, false, ui->scale);
    ui->fLine = makeFont(13, true, ui->scale);
    ui->fSmall = makeFont(11.5, false, ui->scale);
    ui->fBtn = makeFont(12.5, false, ui->scale);
    SetTimer(hwnd, 1, 40, NULL);
    return 0;
  }
  case WM_TIMER: {
    bool done;
    {
      std::lock_guard<std::mutex> lk(ui->sh->m);
      done = ui->sh->done;
    }
    if (done) {
      KillTimer(hwnd, 1);
      EndDialog(hwnd, 1);
      return 0;
    }
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
#ifdef _WIN32
    // draw off-screen first, so the animation does not flicker
    RECT cr;
    GetClientRect(hwnd, &cr);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(hwnd, mem, ui);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
#else
    paint(hwnd, dc, ui);
#endif
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_LBUTTONUP: {
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    if (PtInRect(&ui->cancelRc, p)) ui->sh->cancel = true;
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == IDCANCEL) ui->sh->cancel = true; // Esc
    return 0;
  case WM_CLOSE:
    ui->sh->cancel = true; // the window closes once the worker has stopped
    return 0;
  case WM_DESTROY:
    if (ui) {
      DeleteObject(ui->fTitle); DeleteObject(ui->fTag); DeleteObject(ui->fLogo); DeleteObject(ui->fLine);
      DeleteObject(ui->fSmall); DeleteObject(ui->fBtn);
    }
    return 0;
  }
  return 0;
}

#ifdef _WIN32
// an empty dialog template, built in memory (no .rc file needed)
struct DlgTemplate {
  DLGTEMPLATE t;
  WORD menu, cls;
  WCHAR title[8];
};
#endif

} // namespace

bool runWithProgress(HWND parent, const std::function<bool(const Progress &)> &work, bool *cancelled) {
  Shared sh;
  Progress prog = [&sh](double f, const std::string &what) {
    std::lock_guard<std::mutex> lk(sh.m);
    sh.fraction = f;
    sh.detail = what;
    return !sh.cancel.load();
  };
  std::thread th([&]() {
    bool r = work(prog);
    std::lock_guard<std::mutex> lk(sh.m);
    sh.result = r;
    sh.done = true;
    sh.cv.notify_all();
  });

  // small AAFs finish before the window would even appear
  {
    std::unique_lock<std::mutex> lk(sh.m);
    sh.cv.wait_for(lk, std::chrono::milliseconds(400), [&] { return sh.done; });
  }
  bool needUi;
  {
    std::lock_guard<std::mutex> lk(sh.m);
    needUi = !sh.done;
  }
  if (needUi) {
    Ui ui;
    ui.sh = &sh;
    ui.t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
    DlgTemplate dt;
    memset(&dt, 0, sizeof(dt));
    dt.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT * 0;
    dt.t.cx = 300;
    dt.t.cy = 180;
    wcscpy(dt.title, L"OLAAF");
    DialogBoxIndirectParam((HINSTANCE)g_hInst, &dt.t, parent, (DLGPROC)dlgProc, (LPARAM)&ui);
#else
    DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_OLAAF_PROGRESS), parent, dlgProc, (LPARAM)&ui);
#endif
  }
  th.join();
  if (cancelled) *cancelled = sh.cancel.load();
  return sh.result;
}

} // namespace olaaf
