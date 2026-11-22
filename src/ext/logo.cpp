// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
#include <cmath>
#include <cstring>
#include "logo.h"
#undef min
#undef max

namespace olaaf {
namespace {
struct Pt { float x, y; };
// outline of the dmajorsound mark, traced from the logo artwork; height 1.0
const Pt kMark[] = {{0.0000f,0.8156f},{0.0125f,0.8531f},{0.0375f,0.8906f},{0.0656f,0.9187f},{0.1062f,0.9469f},{0.1375f,0.9625f},{0.1906f,0.9812f},{0.2313f,0.9906f},{0.2875f,0.9938f},{0.2906f,0.9969f},{0.6719f,0.9969f},{0.6813f,0.9938f},{0.6937f,0.9844f},{0.7000f,0.9719f},{0.7000f,0.0781f},{0.7063f,0.0656f},{0.7219f,0.0531f},{0.7562f,0.0531f},{0.7719f,0.0656f},{0.7781f,0.0781f},{0.7781f,0.4469f},{0.7812f,0.4562f},{0.8031f,0.4719f},{1.0719f,0.4719f},{1.0875f,0.4594f},{1.0906f,0.4500f},{1.0906f,0.0750f},{1.1125f,0.0531f},{1.1438f,0.0531f},{1.1656f,0.0719f},{1.1687f,0.0813f},{1.1687f,0.4469f},{1.1719f,0.4531f},{1.1969f,0.4719f},{1.4531f,0.4719f},{1.4625f,0.4688f},{1.4781f,0.4531f},{1.4781f,0.1781f},{1.4719f,0.1562f},{1.4594f,0.1313f},{1.4344f,0.0969f},{1.4031f,0.0688f},{1.3750f,0.0500f},{1.3313f,0.0281f},{1.2844f,0.0125f},{1.2375f,0.0031f},{1.2188f,0.0031f},{1.2156f,0.0000f},{0.4094f,0.0000f},{0.3906f,0.0156f},{0.3875f,0.0250f},{0.3875f,0.9187f},{0.3688f,0.9406f},{0.3375f,0.9437f},{0.3156f,0.9281f},{0.3094f,0.9125f},{0.3094f,0.5469f},{0.3031f,0.5344f},{0.2875f,0.5250f},{0.2594f,0.5250f},{0.2562f,0.5281f},{0.2219f,0.5312f},{0.1500f,0.5531f},{0.0969f,0.5813f},{0.0719f,0.6000f},{0.0375f,0.6344f},{0.0125f,0.6750f},{0.0000f,0.7125f}};
const float kMarkW = 1.4812f;
} // namespace

int drawDmajorLogo(HDC dc, int right, int midY, int markH, COLORREF markColor, COLORREF textColor, HFONT font) {
  // the mark at the right edge
  const int np = (int)(sizeof(kMark) / sizeof(kMark[0]));
  POINT pts[sizeof(kMark) / sizeof(kMark[0])];
  int markW = (int)std::lround(markH * kMarkW);
  int left = right - markW, top = midY - markH / 2;
  for (int i = 0; i < np; i++) {
    pts[i].x = left + (int)std::lround(kMark[i].x * markH);
    pts[i].y = top + (int)std::lround(kMark[i].y * markH);
  }
  HBRUSH br = CreateSolidBrush(markColor);
  HPEN pen = CreatePen(PS_SOLID, 1, markColor);
  HGDIOBJ oldB = SelectObject(dc, br), oldP = SelectObject(dc, pen);
  Polygon(dc, pts, np);
  SelectObject(dc, oldB);
  SelectObject(dc, oldP);
  DeleteObject(br);
  DeleteObject(pen);

  // "dmajorsound.com" in small type, left of the mark
  const char *url = "dmajorsound.com";
  HGDIOBJ oldF = SelectObject(dc, font);
  RECT m = {0, 0, 2000, 200};
  DrawText(dc, url, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  int tw = m.right - m.left;
  int tr = left - (int)std::lround(markH * 0.5);
  // DrawText centers the whole line box (ascent + descent); lowercase type then sits optically high,
  // so it is nudged down a little to line up with the middle of the mark
  int nudge = (int)std::lround(markH * 0.07);
  RECT rc = {tr - tw - 2, midY - markH + nudge, tr, midY + markH + nudge};
  SetTextColor(dc, textColor);
  SetBkMode(dc, TRANSPARENT);
  DrawText(dc, url, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
  SelectObject(dc, oldF);
  return rc.left;
}
} // namespace olaaf
