// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
#include <cmath>
#include <cstring>
#include "logo.h"
#include "theme.h"
#include "round.h"
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
  // the mark at the right edge, drawn smooth (anti-aliased, sub-pixel) so it stays clean when small
  const int np = (int)(sizeof(kMark) / sizeof(kMark[0]));
  double xy[2 * sizeof(kMark) / sizeof(kMark[0])];
  double markW = markH * kMarkW;
  double leftD = right - markW, topD = midY - markH / 2.0;
  for (int i = 0; i < np; i++) {
    xy[2 * i] = leftD + kMark[i].x * markH;
    xy[2 * i + 1] = topD + kMark[i].y * markH;
  }
  olPolyFill(dc, xy, np, markColor);
  int left = (int)std::lround(leftD);

  // "dmajorsound.com" in small type, left of the mark
  const char *url = "dmajorsound.com";
  HGDIOBJ oldF = SelectObject(dc, font);
  RECT m = {0, 0, 2000, 200};
  olDrawText(dc, url, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  int tw = m.right - m.left;
  int tr = left - (int)std::lround(markH * 0.5);
  // put the middle of the capital / ascender height on the middle of the mark: baseline = mid + cap/2
  TEXTMETRIC tm;
  memset(&tm, 0, sizeof(tm));
  GetTextMetrics(dc, &tm);
  int em = tm.tmHeight - tm.tmInternalLeading;
  if (em <= 0) em = markH;
  int cap = (int)std::lround(em * 0.71);
  int baseline = midY + (cap + 1) / 2;
  int top2 = baseline - (tm.tmAscent > 0 ? tm.tmAscent : em);
  RECT rc = {tr - tw - 2, top2, tr, top2 + (tm.tmHeight > 0 ? tm.tmHeight : em + em / 4)};
  SetTextColor(dc, textColor);
  SetBkMode(dc, TRANSPARENT);
  olDrawText(dc, url, &rc, DT_SINGLELINE | DT_TOP | DT_RIGHT | DT_NOPREFIX);
  SelectObject(dc, oldF);
  return rc.left;
}
} // namespace olaaf
