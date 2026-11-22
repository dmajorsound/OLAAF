// Renders the olAAF import dialog to SVG without REAPER (layout check).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/importer.h"
#include "../src/core/util.h"
#include "../third_party/reaper-sdk/WDL/swell/swell.h"
#undef min
#undef max
namespace olaaf { void dialogPaintTest(Importer &, ImportOptions &, HDC, int, const char *); }
using namespace olaaf;

struct Font { int h; bool bold; };
struct Obj { int kind; int col; Font f; };
static std::string svg;
static int curText = 0; static Font curFont{13, false}; static int curPen = 0;
static int clipN = 0;
static std::string hex(int c) { char b[8]; snprintf(b, 8, "#%06x", c & 0xffffff); return b; }
static std::string esc(const char *s) { std::string o; for (; *s; s++) { if (*s=='&') o+="&amp;"; else if (*s=='<') o+="&lt;"; else if (*s=='>') o+="&gt;"; else o+=*s; } return o; }

static HBRUSH mBrush(int c) { Obj *o = new Obj{1, c, {}}; return (HBRUSH)o; }
static HPEN mPen(int, int, int c) { Obj *o = new Obj{2, c, {}}; return (HPEN)o; }
static HFONT mFont(int h, int, int, int, int w, char, char, char, char, char, char, char, char, const char *) { Obj *o = new Obj{3, 0, {h < 0 ? -h : h, w >= 700}}; return (HFONT)o; }
static HGDIOBJ mSelect(HDC, HGDIOBJ g) { Obj *o = (Obj *)g; if (o && o->kind == 3) curFont = o->f; if (o && o->kind == 2) curPen = o->col; return g; }
static void mDelete(HGDIOBJ) {}
static void mFill(HDC, const RECT *r, HBRUSH b) { Obj *o = (Obj *)b; char t[256]; snprintf(t, 256, "<rect x='%ld' y='%ld' width='%ld' height='%ld' fill='%s'/>\n", r->left, r->top, r->right - r->left, r->bottom - r->top, hex(o->col).c_str()); svg += t; }
static int mx, my;
static void mMove(HDC, int x, int y, POINT *) { mx = x; my = y; }
static void mLine(HDC, int x, int y) { char t[256]; snprintf(t, 256, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='1'/>\n", mx, my, x, y, hex(curPen).c_str()); svg += t; mx = x; my = y; }
static void mTextColor(HDC, int c) { curText = c; }
static void mBk(HDC, int) {}
static int mDraw(HDC, const char *buf, int, RECT *r, int align) {
  if (align & DT_CALCRECT) { r->right = r->left + (int)(strlen(buf) * curFont.h * 0.6); r->bottom = r->top + curFont.h; return curFont.h; }
  char t[1024]; const char *anchor = (align & DT_CENTER) ? "middle" : (align & DT_RIGHT) ? "end" : "start";
  int x = (align & DT_CENTER) ? (r->left + r->right) / 2 : (align & DT_RIGHT) ? r->right : r->left;
  clipN++;
  snprintf(t, 1024, "<clipPath id='c%d'><rect x='%ld' y='%ld' width='%ld' height='%ld'/></clipPath><text clip-path='url(#c%d)' x='%d' y='%ld' font-family='Helvetica, Arial, sans-serif' font-size='%d' font-weight='%s' fill='%s' text-anchor='%s' dominant-baseline='central'>",
           clipN, r->left, r->top, r->right - r->left, r->bottom - r->top, clipN, x, (r->top + r->bottom) / 2, curFont.h, curFont.bold ? "bold" : "normal", hex(curText).c_str(), anchor);
  svg += t; svg += esc(buf); svg += "</text>\n"; return 0; }
static void mClient(HWND, RECT *r) { r->left = r->top = 0; r->right = 1180; r->bottom = 760; }
static BOOL mPt(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }

int main(int argc, char **argv) {
  CreateSolidBrush = mBrush; CreatePen = mPen; CreateFont = mFont; SelectObject = mSelect; DeleteObject = mDelete;
  SWELL_FillRect = mFill; MoveToEx = mMove; SWELL_LineTo = mLine; SetTextColor = mTextColor; SetBkMode = mBk;
  SWELL_DrawText = mDraw; GetClientRect = mClient; SWELL_PtInRect = mPt;
  Importer imp; ImportOptions opt; std::string err;
  if (!imp.load(argv[1], opt, &err)) { printf("%s\n", err.c_str()); return 1; }
  opt.destDir = "/Users/dmajor/Documents/REAPER Media";
  imp.locateMedia(opt); imp.setMergeStereo(true);
  if (imp.session().tracks.size() > 2) imp.session().tracks[2].color = 0xD9822B;
  if (imp.session().tracks.size() > 4) imp.session().tracks[4].import = false;
  int pal = argc > 3 ? atoi(argv[3]) : -1;
  svg = "<svg xmlns='http://www.w3.org/2000/svg' width='1180' height='760'>\n";
  dialogPaintTest(imp, opt, (HDC)1, pal, nullptr);
  svg += "</svg>\n";
  FILE *f = fopen(argv[2], "wb"); fwrite(svg.data(), 1, svg.size(), f); fclose(f);
  return 0;
}
