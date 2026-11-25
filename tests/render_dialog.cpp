#include <cmath>
// Renders the olAAF import dialog to SVG without REAPER (layout check).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/importer.h"
#include "../src/core/util.h"
#include "../src/ext/theme.h"
#include "../third_party/reaper-sdk/WDL/swell/swell.h"
#undef min
#undef max
namespace olaaf { void dialogPaintTest(Importer &, ImportOptions &, HDC, int, const char *); }
using namespace olaaf;

struct Font { int h; bool bold; bool mono = false; };
struct Obj { int kind; int col; Font f; };
static std::string svg;
static int curText = 0; static Font curFont{13, false}; static int curPen = 0; static int curBrush = 0;
static int clipN = 0;
static std::string hex(int c) { char b[8]; snprintf(b, 8, "#%06x", c & 0xffffff); return b; }
static std::string esc(const char *s) { std::string o; for (; *s; s++) { if (*s=='&') o+="&amp;"; else if (*s=='<') o+="&lt;"; else if (*s=='>') o+="&gt;"; else o+=*s; } return o; }

static HBRUSH mBrush(int c) { Obj *o = new Obj{1, c, {}}; return (HBRUSH)o; }
static int penW = 1, curPenW = 1;
static HPEN mPen(int, int w, int c) { Obj *o = new Obj{2, c, {w, false}}; return (HPEN)o; }
static HFONT mFont(int h, int, int, int, int w, char, char, char, char, char, char, char, char, const char *face) { Obj *o = new Obj{3, 0, {h < 0 ? -h : h, w >= 700, face && (strstr(face, "Mono") || strstr(face, "Menlo") || strstr(face, "Consolas"))}}; return (HFONT)o; }
static std::string famSans() { const char *e = getenv("OLAAF_FONT"); return e ? e : "Helvetica, Arial, sans-serif"; }
static std::string famMono() { const char *e = getenv("OLAAF_MONO"); return e ? e : "Menlo, monospace"; }
static HGDIOBJ mSelect(HDC, HGDIOBJ g) { Obj *o = (Obj *)g; if (o && o->kind == 3) curFont = o->f; if (o && o->kind == 2) { curPen = o->col; curPenW = o->f.h; } if (o && o->kind == 1) curBrush = o->col; return g; }
static void mDelete(HGDIOBJ) {}
static void mFill(HDC, const RECT *r, HBRUSH b) { Obj *o = (Obj *)b; char t[256]; snprintf(t, 256, "<rect x='%ld' y='%ld' width='%ld' height='%ld' fill='%s'/>\n", r->left, r->top, r->right - r->left, r->bottom - r->top, hex(o->col).c_str()); svg += t; }
static int mx, my;
static void mMove(HDC, int x, int y, POINT *) { mx = x; my = y; }
static void mLine(HDC, int x, int y) { char t[256]; snprintf(t, 256, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='%d' stroke-linecap='square'/>\n", mx, my, x, y, hex(curPen).c_str(), curPenW); svg += t; mx = x; my = y; }
static void mTextColor(HDC, int c) { curText = c; }
static void mBk(HDC, int) {}
// IBM Plex Sans advance widths (1/1000 em) for ASCII, so the mock measures like the real fonts
static const short kPlexR[95] = {236,284,419,713,598,927,694,242,335,335,450,600,272,399,272,383,600,600,600,600,600,600,600,600,600,600,292,292,600,600,600,477,891,641,653,621,671,583,559,695,707,400,510,634,501,812,707,708,606,708,640,581,572,678,609,891,613,593,580,317,383,317,600,565,600,534,580,503,580,549,324,528,568,250,250,527,272,873,568,560,580,580,367,487,351,568,492,768,507,499,464,343,314,343,600};
static const short kPlexB[95] = {236,320,493,632,601,974,721,268,338,338,601,600,310,403,310,460,600,600,600,600,600,600,600,600,600,600,330,330,600,600,600,500,903,685,667,651,697,607,585,719,724,432,559,696,530,819,724,714,656,714,674,624,584,694,650,973,673,649,607,334,460,334,600,556,600,569,608,517,608,562,361,552,596,286,286,577,303,894,596,564,608,608,404,504,383,596,538,841,560,534,518,372,402,372,600};
static int textWidth(const char *buf) {
  double w = 0;
  for (const unsigned char *p = (const unsigned char *)buf; *p; p++) {
    if ((*p & 0xC0) == 0x80) continue; // UTF-8 continuation
    int c = *p;
    double a = curFont.mono ? 600 : (c >= 32 && c < 127 ? (curFont.bold ? kPlexB : kPlexR)[c - 32] : 600);
    w += a * curFont.h / 1000.0;
  }
  return (int)std::ceil(w);
}
static int mDraw(HDC, const char *buf, int, RECT *r, int align) {
  if (align & DT_CALCRECT) { r->right = r->left + textWidth(buf); r->bottom = r->top + curFont.h; return curFont.h; }
  char t[1024]; const char *anchor = (align & DT_CENTER) ? "middle" : (align & DT_RIGHT) ? "end" : "start";
  int x = (align & DT_CENTER) ? (r->left + r->right) / 2 : (align & DT_RIGHT) ? r->right : r->left;
  clipN++;
  snprintf(t, 1024, "<clipPath id='c%d'><rect x='%ld' y='%ld' width='%ld' height='%ld'/></clipPath><text clip-path='url(#c%d)' x='%d' y='%ld' font-family='%s' font-size='%d' font-weight='%s' fill='%s' text-anchor='%s' dominant-baseline='%s'>",
           clipN, r->left, r->top, r->right - r->left, r->bottom - r->top, clipN, x, (align & DT_VCENTER) ? (long)std::lround((r->top + r->bottom) / 2.0 + curFont.h * 0.36) : r->top + (long)(curFont.h * 0.95), (curFont.mono ? famMono() : famSans()).c_str(), curFont.h, curFont.bold ? "bold" : "normal", hex(curText).c_str(), anchor, "alphabetic");
  svg += t; svg += esc(buf); svg += "</text>\n"; return 0; }
static void mPoly(HDC, POINT *p, int n) { svg += "<polygon points='"; char t[32]; for (int i = 0; i < n; i++) { snprintf(t, 32, "%ld,%ld ", p[i].x, p[i].y); svg += t; } svg += "' fill='" + hex(curBrush) + "'/>\n"; }
static BOOL mTm(HDC, TEXTMETRIC *tm) { tm->tmHeight = (int)(curFont.h * 1.2); tm->tmInternalLeading = tm->tmHeight - curFont.h; tm->tmAscent = (int)(curFont.h * 0.95); tm->tmDescent = tm->tmHeight - tm->tmAscent; return 1; }
static int zoomK() { const char *e = getenv("OLAAF_ZOOM"); return e ? atoi(e) : 1; }
static void mClient(HWND, RECT *r) { r->left = r->top = 0; r->right = 1180 * zoomK(); r->bottom = 742 * zoomK(); }
static BOOL mPt(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }

int main(int argc, char **argv) {
  CreateSolidBrush = mBrush; CreatePen = mPen; CreateFont = mFont; SelectObject = mSelect; DeleteObject = mDelete;
  SWELL_FillRect = mFill; MoveToEx = mMove; SWELL_LineTo = mLine; SetTextColor = mTextColor; SetBkMode = mBk;
  SWELL_DrawText = mDraw; GetClientRect = mClient; SWELL_PtInRect = mPt; SWELL_Polygon = mPoly; GetTextMetrics = mTm;
  Importer imp; ImportOptions opt; std::string err;
  if (!imp.load(argv[1], opt, &err)) { printf("%s\n", err.c_str()); return 1; }
  opt.destDir = "/Users/dmajor/Documents/REAPER Media";
  imp.locateMedia(opt); imp.setMergeStereo(true);
  if (getenv("OLAAF_SHOWCASE")) { // pictures for posts: media found, a tidy file name
    for (auto &m : imp.session().media) m.foundPath = argv[1];
    imp.session().aafPath = std::string("/Projects/") + getenv("OLAAF_SHOWCASE");
  }
  if (imp.session().tracks.size() > 2) imp.session().tracks[2].color = 0xD9822B;
  if (imp.session().tracks.size() > 4) imp.session().tracks[4].import = false;
  int pal = argc > 3 ? atoi(argv[3]) : -1;
  svg = "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(1180 * zoomK()) + "' height='" + std::to_string(742 * zoomK()) + "'>\n";
  if (argc > 4) olaaf::setDarkTheme(true); else olaaf::setThemeMode(2); // dark = REAPER 7 default theme
  dialogPaintTest(imp, opt, (HDC)1, pal, nullptr);
  svg += "</svg>\n";
  FILE *f = fopen(argv[2], "wb"); fwrite(svg.data(), 1, svg.size(), f); fclose(f);
  return 0;
}
