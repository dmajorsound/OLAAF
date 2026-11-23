// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#ifdef _WIN32
#include <windows.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>
#endif

#include "round.h"

#include <cmath>
#include <vector>

namespace olaaf {

#ifdef _WIN32
static void gdipStart() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token, &in, NULL);
  }
}
static Gdiplus::Color gc(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
static void addRound(Gdiplus::GraphicsPath &p, float l, float t, float r, float b, float rad) {
  float d = std::min(rad * 2, std::min(r - l, b - t));
  if (d < 1) {
    p.AddRectangle(Gdiplus::RectF(l, t, r - l, b - t));
    return;
  }
  p.AddArc(l, t, d, d, 180, 90);
  p.AddArc(r - d, t, d, d, 270, 90);
  p.AddArc(r - d, b - d, d, d, 0, 90);
  p.AddArc(l, b - d, d, d, 90, 90);
  p.CloseFigure();
}

void olRoundFill(HDC dc, double l, double t, double r, double b, double rad, COLORREF c) {
  if (r <= l || b <= t) return;
  gdipStart();
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::GraphicsPath p;
  addRound(p, (float)l, (float)t, (float)r, (float)b, (float)rad);
  Gdiplus::SolidBrush br(gc(c));
  g.FillPath(&br, &p);
}

void olRoundCorners(HDC dc, double l, double t, double r, double b, double rad, COLORREF bg, COLORREF line) {
  gdipStart();
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  float L = (float)l, T = (float)t, R = (float)r, B = (float)b, d = (float)rad * 2;
  // the four corner pieces: a square minus a quarter circle
  Gdiplus::SolidBrush br(gc(bg));
  float cs[4][2] = {{L, T}, {R - (float)rad, T}, {R - (float)rad, B - (float)rad}, {L, B - (float)rad}};
  float st[4] = {180, 270, 0, 90};
  float ax[4] = {L, R - d, R - d, L}, ay[4] = {T, T, B - d, B - d};
  for (int i = 0; i < 4; i++) {
    Gdiplus::GraphicsPath p(Gdiplus::FillModeAlternate);
    p.AddRectangle(Gdiplus::RectF(cs[i][0], cs[i][1], (float)rad, (float)rad));
    p.AddPie(ax[i], ay[i], d, d, st[i], 90);
    g.FillPath(&br, &p);
  }
  Gdiplus::GraphicsPath p;
  addRound(p, L + 0.5f, T + 0.5f, R - 0.5f, B - 0.5f, (float)rad - 0.5f);
  Gdiplus::Pen pen(gc(line), 1.0f);
  g.DrawPath(&pen, &p);
}
#else
// SWELL draws polygons with Core Graphics (smooth edges on macOS)
static void arc(std::vector<POINT> &pts, double cx, double cy, double rad, double a0) {
  const int n = 8;
  for (int i = 0; i <= n; i++) {
    double a = (a0 + 90.0 * i / n) * 3.14159265358979323846 / 180.0;
    pts.push_back({(LONG)std::lround(cx + rad * std::cos(a)), (LONG)std::lround(cy + rad * std::sin(a))});
  }
}
static void poly(HDC dc, std::vector<POINT> &pts, COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, 1, c);
  HBRUSH br = CreateSolidBrush(c);
  HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
  Polygon(dc, pts.data(), (int)pts.size());
  SelectObject(dc, op);
  SelectObject(dc, ob);
  DeleteObject(pen);
  DeleteObject(br);
}

void olRoundFill(HDC dc, double l, double t, double r, double b, double rad, COLORREF c) {
  if (r <= l || b <= t) return;
  rad = std::min(rad, std::min(r - l, b - t) / 2);
  std::vector<POINT> pts;
  arc(pts, l + rad, t + rad, rad, 180);
  arc(pts, r - rad, t + rad, rad, 270);
  arc(pts, r - rad, b - rad, rad, 0);
  arc(pts, l + rad, b - rad, rad, 90);
  poly(dc, pts, c);
}

void olRoundCorners(HDC dc, double l, double t, double r, double b, double rad, COLORREF bg, COLORREF line) {
  double cx[4] = {l + rad, r - rad, r - rad, l + rad}, cy[4] = {t + rad, t + rad, b - rad, b - rad};
  double kx[4] = {l, r, r, l}, ky[4] = {t, t, b, b};
  double a0[4] = {180, 270, 0, 90};
  for (int i = 0; i < 4; i++) {
    std::vector<POINT> pts;
    pts.push_back({(LONG)std::lround(kx[i]), (LONG)std::lround(ky[i])});
    arc(pts, cx[i], cy[i], rad, a0[i]);
    poly(dc, pts, bg);
  }
  HPEN pen = CreatePen(PS_SOLID, 1, line);
  HGDIOBJ op = SelectObject(dc, pen);
  for (int i = 0; i < 4; i++) {
    std::vector<POINT> pts;
    arc(pts, cx[i], cy[i], rad - 0.5, a0[i]);
    MoveToEx(dc, pts[0].x, pts[0].y, NULL);
    for (size_t k = 1; k < pts.size(); k++) LineTo(dc, pts[k].x, pts[k].y);
  }
  // straight edges
  MoveToEx(dc, (int)std::lround(l + rad), (int)std::lround(t), NULL);
  LineTo(dc, (int)std::lround(r - rad), (int)std::lround(t));
  MoveToEx(dc, (int)std::lround(r - 1), (int)std::lround(t + rad), NULL);
  LineTo(dc, (int)std::lround(r - 1), (int)std::lround(b - rad));
  MoveToEx(dc, (int)std::lround(l + rad), (int)std::lround(b - 1), NULL);
  LineTo(dc, (int)std::lround(r - rad), (int)std::lround(b - 1));
  MoveToEx(dc, (int)std::lround(l), (int)std::lround(t + rad), NULL);
  LineTo(dc, (int)std::lround(l), (int)std::lround(b - rad));
  SelectObject(dc, op);
  DeleteObject(pen);
}
#endif

} // namespace olaaf
