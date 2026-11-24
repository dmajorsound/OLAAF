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

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

#include <algorithm>
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
void olPolyFill(HDC dc, const double *xy, int n, COLORREF c) {
  if (n < 3) return;
  gdipStart();
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  std::vector<Gdiplus::PointF> p((size_t)n);
  for (int i = 0; i < n; i++) p[(size_t)i] = Gdiplus::PointF((float)xy[2 * i], (float)xy[2 * i + 1]);
  Gdiplus::SolidBrush br(gc(c));
  g.FillPolygon(&br, p.data(), n);
}

void olPolyline(HDC dc, const double *xy, int n, COLORREF c, double width) {
  if (n < 2) return;
  gdipStart();
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  std::vector<Gdiplus::PointF> p((size_t)n);
  for (int i = 0; i < n; i++) p[(size_t)i] = Gdiplus::PointF((float)xy[2 * i], (float)xy[2 * i + 1]);
  Gdiplus::Pen pen(gc(c), (float)width);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  g.DrawLines(&pen, p.data(), n);
}
#else
#ifdef __APPLE__
// macOS: draw with Core Graphics directly, with sub-pixel coordinates (sharp on Retina screens);
// SWELL's DC coordinates map 1:1 to the context
static CGContextRef cgOf(HDC dc) { return (CGContextRef)SWELL_GetCtxGC(dc); }
static bool cgPath(HDC dc, const double *xy, int n, COLORREF c, bool fill, double width) {
  CGContextRef ctx = cgOf(dc);
  if (!ctx || n < 2) return false;
  CGContextSaveGState(ctx);
  CGContextSetShouldAntialias(ctx, true);
  CGContextBeginPath(ctx);
  CGContextMoveToPoint(ctx, xy[0], xy[1]);
  for (int i = 1; i < n; i++) CGContextAddLineToPoint(ctx, xy[2 * i], xy[2 * i + 1]);
  double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
  if (fill) {
    CGContextClosePath(ctx);
    CGContextSetRGBFillColor(ctx, r, g, b, 1.0);
    CGContextFillPath(ctx);
  } else {
    CGContextSetRGBStrokeColor(ctx, r, g, b, 1.0);
    CGContextSetLineWidth(ctx, width);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);
    CGContextStrokePath(ctx);
  }
  CGContextRestoreGState(ctx);
  return true;
}
static void roundPts(std::vector<double> &xy, double l, double t, double r, double b, double rad) {
  rad = std::max(0.0, std::min(rad, std::min(r - l, b - t) / 2));
  double cx[4] = {l + rad, r - rad, r - rad, l + rad}, cy[4] = {t + rad, t + rad, b - rad, b - rad};
  double a0[4] = {180, 270, 0, 90};
  for (int k = 0; k < 4; k++)
    for (int i = 0; i <= 10; i++) {
      double a = (a0[k] + 9.0 * i) * 3.14159265358979323846 / 180.0;
      xy.push_back(cx[k] + rad * std::cos(a));
      xy.push_back(cy[k] + rad * std::sin(a));
    }
}
#endif
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
#ifdef __APPLE__
  {
    std::vector<double> xy;
    roundPts(xy, l, t, r, b, rad);
    if (cgPath(dc, xy.data(), (int)xy.size() / 2, c, true, 0)) return;
  }
#endif
  rad = std::min(rad, std::min(r - l, b - t) / 2);
  std::vector<POINT> pts;
  arc(pts, l + rad, t + rad, rad, 180);
  arc(pts, r - rad, t + rad, rad, 270);
  arc(pts, r - rad, b - rad, rad, 0);
  arc(pts, l + rad, b - rad, rad, 90);
  poly(dc, pts, c);
}

void olRoundCorners(HDC dc, double l, double t, double r, double b, double rad, COLORREF bg, COLORREF line) {
#ifdef __APPLE__
  if (cgOf(dc)) {
    double cx[4] = {l + rad, r - rad, r - rad, l + rad}, cy[4] = {t + rad, t + rad, b - rad, b - rad};
    double kx[4] = {l, r, r, l}, ky[4] = {t, t, b, b};
    double a0[4] = {180, 270, 0, 90};
    for (int k = 0; k < 4; k++) {
      std::vector<double> xy = {kx[k], ky[k]};
      for (int i = 0; i <= 10; i++) {
        double a = (a0[k] + 9.0 * i) * 3.14159265358979323846 / 180.0;
        xy.push_back(cx[k] + rad * std::cos(a));
        xy.push_back(cy[k] + rad * std::sin(a));
      }
      cgPath(dc, xy.data(), (int)xy.size() / 2, bg, true, 0);
    }
    std::vector<double> o;
    roundPts(o, l + 0.5, t + 0.5, r - 0.5, b - 0.5, rad - 0.5);
    o.push_back(o[0]);
    o.push_back(o[1]);
    cgPath(dc, o.data(), (int)o.size() / 2, line, false, 1.0);
    return;
  }
#endif
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

void olPolyFill(HDC dc, const double *xy, int n, COLORREF c) {
  if (n < 3) return;
#ifdef __APPLE__
  if (cgPath(dc, xy, n, c, true, 0)) return;
#endif
  std::vector<POINT> pts((size_t)n);
  for (int i = 0; i < n; i++) pts[(size_t)i] = {(LONG)std::lround(xy[2 * i]), (LONG)std::lround(xy[2 * i + 1])};
  poly(dc, pts, c);
}

void olPolyline(HDC dc, const double *xy, int n, COLORREF c, double width) {
  if (n < 2) return;
#ifdef __APPLE__
  if (cgPath(dc, xy, n, c, false, width)) return;
#endif
  HPEN pen = CreatePen(PS_SOLID, std::max(1, (int)std::lround(width)), c);
  HGDIOBJ op = SelectObject(dc, pen);
  MoveToEx(dc, (int)std::lround(xy[0]), (int)std::lround(xy[1]), NULL);
  for (int i = 1; i < n; i++) LineTo(dc, (int)std::lround(xy[2 * i]), (int)std::lround(xy[2 * i + 1]));
  SelectObject(dc, op);
  DeleteObject(pen);
}
#endif

} // namespace olaaf
