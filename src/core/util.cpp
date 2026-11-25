// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include <cstdlib>
#include <vector>
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

bool fileStamp(const std::string &path, uint64_t *size, int64_t *changed) {
  std::error_code ec;
  auto sz = fs::file_size(P(path), ec);
  if (ec) return false;
  auto t = fs::last_write_time(P(path), ec);
  if (ec) return false;
  *size = (uint64_t)sz;
  *changed = (int64_t)t.time_since_epoch().count();
  return true;
}

bool renamePath(const std::string &from, const std::string &to) {
  std::error_code ec;
  fs::rename(P(from), P(to), ec);
  return !ec;
}

void removePath(const std::string &path) {
  std::error_code ec;
  fs::remove(P(path), ec);
}

void trimFolder(const std::string &dir, uint64_t maxBytes) {
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, std::pair<fs::path, uint64_t>>> files;
  uint64_t total = 0;
  for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    uint64_t sz = (uint64_t)it->file_size(e2);
    if (e2) continue;
    files.push_back({it->last_write_time(e2), {it->path(), sz}});
    total += sz;
  }
  if (total <= maxBytes) return;
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  for (auto &f : files) {
    if (total <= maxBytes) break;
    std::error_code e3;
    if (fs::remove(f.second.first, e3)) total -= f.second.second;
  }
}

void trimSubfolders(const std::string &dir, uint64_t maxBytes) {
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, std::pair<fs::path, uint64_t>>> dirs;
  uint64_t total = 0;
  for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    uint64_t sz = 0;
    fs::file_time_type newest = it->last_write_time(e2);
    for (fs::recursive_directory_iterator f(it->path(), e2), fe; !e2 && f != fe; f.increment(e2)) {
      std::error_code e3;
      if (!f->is_regular_file(e3)) continue;
      sz += (uint64_t)f->file_size(e3);
      auto t = f->last_write_time(e3);
      if (!e3 && t > newest) newest = t;
    }
    dirs.push_back({newest, {it->path(), sz}});
    total += sz;
  }
  if (total <= maxBytes) return;
  std::sort(dirs.begin(), dirs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  for (auto &d : dirs) {
    if (total <= maxBytes) break;
    std::error_code e4;
    fs::remove_all(d.second.first, e4);
    if (!e4) total -= d.second.second;
  }
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

void indexFolder(const std::string &dir, int maxDepth, std::vector<std::pair<std::string, std::string>> *out,
                 size_t maxFiles) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(P(dir), ec)) return;
  fs::recursive_directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end;
  size_t seen = 0;
  for (; it != end && !ec; it.increment(ec)) {
    if (++seen > maxFiles) break; // a whole disk is not a media folder
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
  // accepts "10:00:00:00", "10;00;00;00", "10.00.00.00" and plain digits ("10000000", "1000" = 00:00:10:00);
  // fields are filled from the right (frames, seconds, minutes, hours) like a timecode keypad
  std::string t;
  for (char c : s)
    if (c != ' ' && c != '\t') t += c;
  bool neg = !t.empty() && t[0] == '-';
  if (!t.empty() && (t[0] == '-' || t[0] == '+')) t.erase(0, 1);
  if (t.empty()) return false;
  std::vector<std::string> groups(1);
  for (char c : t) {
    if (c >= '0' && c <= '9') groups.back() += c;
    else if (c == ':' || c == ';' || c == '.') groups.emplace_back();
    else return false;
  }
  int v[4] = {0, 0, 0, 0}; // frames, seconds, minutes, hours
  if (groups.size() == 1) {
    std::string d = groups[0];
    if (d.size() > 8) return false;
    d = std::string(8 - d.size(), '0') + d;
    for (int i = 0; i < 4; i++) v[3 - i] = atoi(d.substr((size_t)i * 2, 2).c_str());
  } else {
    if (groups.size() > 4) return false;
    for (size_t i = 0; i < groups.size(); i++) {
      const std::string &g = groups[groups.size() - 1 - i];
      if (g.size() > 3) return false;
      v[i] = g.empty() ? 0 : atoi(g.c_str());
    }
  }
  if (v[1] > 59 || v[2] > 59 || v[0] >= (int)std::ceil(fps)) return false;
  double sec = v[3] * 3600.0 + v[2] * 60.0 + v[1] + v[0] / fps;
  *seconds = neg ? -sec : sec;
  return true;
}

} // namespace olaaf
