// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "util.h"
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace olaaf {

static fs::path P(const std::string &s) {
#if defined(_WIN32)
  return fs::u8path(s);
#else
  return fs::path(s);
#endif
}

static std::string S(const fs::path &p) {
#if defined(_WIN32)
  return p.u8string();
#else
  return p.string();
#endif
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string fileExt(const std::string &path) {
  std::string b = baseName(path);
  size_t d = b.rfind('.');
  return d == std::string::npos ? "" : b.substr(d + 1);
}

std::string baseName(const std::string &path) {
  size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}

std::string stemName(const std::string &path) {
  std::string b = baseName(path);
  size_t d = b.rfind('.');
  return d == std::string::npos ? b : b.substr(0, d);
}

std::string dirName(const std::string &path) {
  size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? "." : path.substr(0, s);
}

std::string joinPath(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  char last = a.back();
  if (last == '/' || last == '\\') return a + b;
#if defined(_WIN32)
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

bool fileExists(const std::string &path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(P(path), ec);
}

bool makeDirs(const std::string &path) {
  std::error_code ec;
  fs::create_directories(P(path), ec);
  return fs::is_directory(P(path), ec);
}

bool copyFile(const std::string &from, const std::string &to) {
  std::error_code ec;
  fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, ec);
  return !ec;
}

std::string safeFileName(const std::string &name) {
  std::string o;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|' || (unsigned char)c < 32)
      o += '_';
    else
      o += c;
  }
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  return o.empty() ? "audio" : o;
}

std::string uniquePath(const std::string &path) {
  if (!fileExists(path)) return path;
  std::string d = dirName(path), st = stemName(path), e = fileExt(path);
  for (int i = 1; i < 100000; i++) {
    std::string p = joinPath(d, st + "-" + std::to_string(i) + (e.empty() ? "" : "." + e));
    if (!fileExists(p)) return p;
  }
  return path;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string uriToPath(const std::string &uri) {
  std::string u = uri;
  if (lower(u.substr(0, 7)) == "file://") {
    u = u.substr(7);
    // drop host part ("localhost/…" or "/…")
    if (!u.empty() && u[0] != '/') {
      size_t s = u.find('/');
      u = s == std::string::npos ? "" : u.substr(s);
    }
  }
  std::string o;
  for (size_t i = 0; i < u.size(); i++) {
    if (u[i] == '%' && i + 2 < u.size() && hexv(u[i + 1]) >= 0 && hexv(u[i + 2]) >= 0) {
      o += (char)(hexv(u[i + 1]) * 16 + hexv(u[i + 2]));
      i += 2;
    } else
      o += u[i];
  }
#if defined(_WIN32)
  // "/E:/Projekte/x.wav" -> "E:/Projekte/x.wav"
  if (o.size() > 2 && o[0] == '/' && o[2] == ':') o = o.substr(1);
#endif
  return o;
}

void indexFolder(const std::string &dir, int maxDepth, std::vector<std::pair<std::string, std::string>> *out) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(P(dir), ec)) return;
  fs::recursive_directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end;
  for (; it != end && !ec; it.increment(ec)) {
    if (it.depth() > maxDepth) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec)) out->push_back({lower(S(it->path().filename())), S(it->path())});
  }
}

std::string strprintf(const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

// ---- model helpers ----

double Session::firstClip() const {
  double f = 0;
  bool any = false;
  for (auto &t : tracks)
    for (auto &c : t.clips)
      if (!any || c.pos < f) {
        f = c.pos;
        any = true;
      }
  return any ? f : start;
}

std::string formatTC(double seconds, double fps) {
  bool neg = seconds < 0;
  if (neg) seconds = -seconds;
  int ifps = (int)std::lround(fps);
  long long frames = (long long)std::floor(seconds * fps + 1e-6);
  int ff = (int)(frames % ifps);
  long long tot = frames / ifps;
  return strprintf("%s%02lld:%02lld:%02lld:%02d", neg ? "-" : "", tot / 3600, (tot / 60) % 60, tot % 60, ff);
}

bool parseTC(const std::string &s, double fps, double *seconds) {
  int h = 0, m = 0, sec = 0, f = 0;
  bool neg = !s.empty() && s[0] == '-';
  const char *p = s.c_str() + (neg || (!s.empty() && s[0] == '+') ? 1 : 0);
  if (sscanf(p, "%d:%d:%d:%d", &h, &m, &sec, &f) != 4 && sscanf(p, "%d:%d:%d;%d", &h, &m, &sec, &f) != 4)
    return false;
  double v = h * 3600.0 + m * 60.0 + sec + f / fps;
  *seconds = neg ? -v : v;
  return true;
}

} // namespace olaaf
