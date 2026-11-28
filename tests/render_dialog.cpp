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
static std::string famSans() { const char *e = getenv("OLAAF_FONT"); return e ? e : "Mulish OLAAF, sans-serif"; }
static std::string famMono() { const char *e = getenv("OLAAF_MONO"); return e ? e : "Red Hat Mono OLAAF, monospace"; }
static HGDIOBJ mSelect(HDC, HGDIOBJ g) { Obj *o = (Obj *)g; if (o && o->kind == 3) curFont = o->f; if (o && o->kind == 2) { curPen = o->col; curPenW = o->f.h; } if (o && o->kind == 1) curBrush = o->col; return g; }
static void mDelete(HGDIOBJ) {}
static void mFill(HDC, const RECT *r, HBRUSH b) { Obj *o = (Obj *)b; char t[256]; snprintf(t, 256, "<rect x='%ld' y='%ld' width='%ld' height='%ld' fill='%s'/>\n", r->left, r->top, r->right - r->left, r->bottom - r->top, hex(o->col).c_str()); svg += t; }
static int mx, my;
static void mMove(HDC, int x, int y, POINT *) { mx = x; my = y; }
static void mLine(HDC, int x, int y) { char t[256]; snprintf(t, 256, "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='%d' stroke-linecap='square'/>\n", mx, my, x, y, hex(curPen).c_str(), curPenW); svg += t; mx = x; my = y; }
static void mTextColor(HDC, int c) { curText = c; }
static void mBk(HDC, int) {}
// Mulish OLAAF advance widths (1/1000 em) for ASCII, so the mock measures like the real fonts
static const short kSansR[95] = {261,228,392,600,600,937,670,221,318,318,454,600,228,428,228,286,600,600,600,600,600,600,600,600,600,600,228,228,600,600,600,447,969,735,680,684,747,588,562,731,766,259,329,633,545,858,747,772,651,772,692,623,604,730,698,1112,652,607,596,317,286,317,600,505,500,600,600,495,600,544,338,603,585,238,238,515,238,878,585,571,600,600,363,496,350,577,531,869,534,531,518,355,264,355,600};
static const short kSansB[95] = {267,237,417,600,600,943,678,231,335,335,456,600,237,433,237,299,600,600,600,600,600,600,600,600,600,600,237,237,600,600,600,455,967,742,685,684,754,591,567,735,770,270,344,648,552,864,750,779,658,779,697,633,611,735,708,1116,661,617,601,334,299,334,600,505,500,609,609,499,609,546,351,612,593,249,248,531,248,888,593,581,609,609,376,498,363,586,537,875,541,536,522,371,272,371,600};
static int textWidth(const char *buf) {
  double w = 0;
  for (const unsigned char *p = (const unsigned char *)buf; *p; p++) {
    if ((*p & 0xC0) == 0x80) continue; // UTF-8 continuation
    int c = *p;
    double a = curFont.mono ? 600 : (c >= 32 && c < 127 ? (curFont.bold ? kSansB : kSansR)[c - 32] : 600);
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
static int cliW() { return getenv("OLAAF_START") ? 760 : 1180; }
static int cliH() { return getenv("OLAAF_START") ? 540 : 742; }
static void mClient(HWND, RECT *r) { r->left = r->top = 0; r->right = cliW() * zoomK(); r->bottom = cliH() * zoomK(); }
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
  svg = "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(cliW() * zoomK()) + "' height='" + std::to_string(cliH() * zoomK()) + "'>\n";
  if (argc > 4) olaaf::setDarkTheme(true); else olaaf::setThemeMode(2); // dark = REAPER 7 default theme
  dialogPaintTest(imp, opt, (HDC)1, pal, nullptr);
  svg += "</svg>\n";
  FILE *f = fopen(argv[2], "wb"); fwrite(svg.data(), 1, svg.size(), f); fclose(f);
  return 0;
}
// the update check lives in update.cpp (REAPER side): stand-ins for the test renderer
namespace olaaf {
int updateCheckState(std::string *, std::string *) { return 2; }
void openWebPage(const std::string &) {}
std::string updateAssetUrl() { return getenv("OLAAF_NOASSET") ? "" : "https://github.com/dmajorsound/OLAAF/releases/download/v0.8.7/OLAAF-0.8.7-mac.dmg"; }
std::string updateFolder() { return "/Users/bernd/Downloads"; }
void setUpdateFolder(const std::string &, bool) {}
void setUpdateFolderSaver(void (*)(const std::string &)) {}
void updateDownloadStart(bool) {}
int updateDownloadState(double *progress, std::string *path, std::string *error) { // OLAAF_DL=1 (42 %), 2, 3
  const char *d = getenv("OLAAF_DL");
  if (progress) *progress = 0.42;
  if (path) *path = "/Users/bernd/Downloads/OLAAF-0.8.7-mac.dmg";
  if (error) *error = "no connection to GitHub";
  return d ? atoi(d) : 0;
}
void openInstaller(const std::string &) {}
void revealInFileBrowser(const std::string &) {}
}
