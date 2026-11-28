// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "update.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#ifdef OLAAF_HAVE_CURL
#include <curl/curl.h>
#endif

namespace olaaf {

static const char *kHost = "api.github.com";
static const char *kPath = "/repos/dmajorsound/OLAAF/releases?per_page=1"; // newest, betas included

static std::atomic<int> g_state{0};
static std::atomic<bool> g_started{false};
static std::mutex g_mu;
static std::string g_tag, g_url, g_asset;
// the download
static std::atomic<int> g_dl{0};
static std::atomic<long long> g_dlGot{0}, g_dlTotal{0};
static std::string g_dlPath, g_dlError, g_folder;
static const char *kAssetBase = "https://github.com/dmajorsound/OLAAF/releases/download/";

#ifdef _WIN32
static bool fetch(std::string *out) {
  bool ok = false;
  HINTERNET s = WinHttpOpen(L"OLAAF/" OLAAF_VERSION_STRING, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return false;
  WinHttpSetTimeouts(s, 4000, 4000, 4000, 6000);
  HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/repos/dmajorsound/OLAAF/releases?per_page=1", NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                  : NULL;
  if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(r, NULL)) {
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (code == 200) {
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(r, &avail) && avail && out->size() < 2000000) {
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), avail, &got) || !got) break;
        out->append(buf.data(), got);
      }
      ok = !out->empty();
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}
#elif defined(OLAAF_HAVE_CURL)
static size_t curlWrite(char *p, size_t a, size_t b, void *u) {
  std::string *s = (std::string *)u;
  if (s->size() > 2000000) return 0;
  s->append(p, a * b);
  return a * b;
}
static bool fetch(std::string *out) {
  CURL *c = curl_easy_init();
  if (!c) return false;
  std::string url = std::string("https://") + kHost + kPath;
  struct curl_slist *hdr = curl_slist_append(NULL, "Accept: application/vnd.github+json");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "OLAAF/" OLAAF_VERSION_STRING);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  return rc == CURLE_OK && code == 200 && !out->empty();
}
#else
static bool fetch(std::string *) { return false; }
#endif

// the first "key": "value" in the JSON text
static std::string jsonString(const std::string &js, const char *key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = js.find(k);
  if (p == std::string::npos) return std::string();
  p = js.find(':', p + k.size());
  if (p == std::string::npos) return std::string();
  p = js.find('"', p);
  if (p == std::string::npos) return std::string();
  size_t e = js.find('"', p + 1);
  if (e == std::string::npos || e - p > 300) return std::string();
  return js.substr(p + 1, e - p - 1);
}

static std::vector<int> versionParts(const std::string &v) {
  std::vector<int> r;
  int cur = -1;
  for (char ch : v) {
    if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
    else if (cur >= 0) {
      r.push_back(cur);
      cur = -1;
      if (ch != '.') break; // "0.7.0-beta": stop at the suffix
    }
  }
  if (cur >= 0) r.push_back(cur);
  while (r.size() < 3) r.push_back(0);
  return r;
}

static bool newer(const std::string &tag) {
  std::vector<int> a = versionParts(tag), b = versionParts(OLAAF_VERSION_STRING);
  return a > b;
}

void updateCheckStart() {
  if (g_started.exchange(true)) return; // once per REAPER session
  std::thread([]() {
    std::string js;
    if (!fetch(&js)) {
      g_state = 2;
      return;
    }
    std::string tag = jsonString(js, "tag_name"), url = jsonString(js, "html_url");
    if (tag.empty() || !newer(tag) || url.compare(0, 19, "https://github.com/") != 0) {
      g_state = 2;
      return;
    }
    // the installer for this system among the release's files (newest release only: per_page=1)
    std::string asset;
    for (size_t p = js.find("\"browser_download_url\""); p != std::string::npos; p = js.find("\"browser_download_url\"", p + 1)) {
      std::string u = jsonString(js.substr(p), "browser_download_url");
      if (u.compare(0, strlen(kAssetBase), kAssetBase) != 0 || u.find("uninstall") != std::string::npos) continue;
#ifdef _WIN32
      const char *want = "-win64.exe";
#elif defined(__APPLE__)
      const char *want = "-mac.dmg";
#else
      const char *want = nullptr;
#endif
      if (want && u.size() > strlen(want) && u.compare(u.size() - strlen(want), std::string::npos, want) == 0) {
        asset = u;
        break;
      }
    }
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_tag = tag;
      g_url = url;
      g_asset = asset;
    }
    g_state = 1;
  }).detach();
}

int updateCheckState(std::string *tag, std::string *url) {
  int s = g_state.load();
  if (s == 1) {
    std::lock_guard<std::mutex> l(g_mu);
    if (tag) *tag = g_tag;
    if (url) *url = g_url;
  }
  return s;
}

// for the GitHub build's check (tests/update_download.cpp): a released installer to fetch
void updateSetAssetForTest(const std::string &url) {
  std::lock_guard<std::mutex> l(g_mu);
  g_asset = url;
}

std::string updateAssetUrl() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_asset;
}

std::string updateFolder() {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_folder.empty()) return g_folder;
  }
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
#else
  const char *home = getenv("HOME");
#endif
  if (!home || !*home) return std::string();
  return std::string(home) + "/Downloads";
}

static void (*g_saveFolder)(const std::string &) = nullptr;
void setUpdateFolderSaver(void (*save)(const std::string &dir)) { g_saveFolder = save; }

void setUpdateFolder(const std::string &dir, bool save) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_folder = dir;
  }
  if (save && g_saveFolder) g_saveFolder(dir);
}

#ifdef _WIN32
static std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  std::wstring w((size_t)std::max(n, 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(wcslen(w.c_str()));
  return w;
}
static FILE *openOut(const std::string &path) { return _wfopen(wide(path).c_str(), L"wb"); }
static bool moveOver(const std::string &from, const std::string &to) {
  return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static void removeFile(const std::string &p) { DeleteFileW(wide(p).c_str()); }
// GET with redirects (github.com sends the file from its download servers), into `f`
static bool download(const std::string &url, FILE *f, std::string *err) {
  URL_COMPONENTSW uc;
  memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  std::wstring wurl = wide(url);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    *err = "bad address";
    return false;
  }
  bool ok = false;
  HINTERNET s = WinHttpOpen(L"OLAAF/" OLAAF_VERSION_STRING, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) {
    *err = "no connection";
    return false;
  }
  WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                  : NULL;
  if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (code == 200) {
      wchar_t len[32];
      DWORD lsz = sizeof(len);
      if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz, WINHTTP_NO_HEADER_INDEX))
        g_dlTotal = _wtoi64(len);
      ok = true;
      std::vector<char> buf(1 << 16);
      for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) {
          ok = false;
          *err = "the connection broke off";
          break;
        }
        if (!got) break;
        if (fwrite(buf.data(), 1, got, f) != got) {
          ok = false;
          *err = "could not write the file";
          break;
        }
        g_dlGot += got;
      }
    } else
      *err = "GitHub answered " + std::to_string(code);
  } else
    *err = "no connection to GitHub";
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}
#else
static FILE *openOut(const std::string &path) { return fopen(path.c_str(), "wb"); }
static bool moveOver(const std::string &from, const std::string &to) { return rename(from.c_str(), to.c_str()) == 0; }
static void removeFile(const std::string &p) { unlink(p.c_str()); }
#ifdef OLAAF_HAVE_CURL
static size_t curlFile(char *p, size_t a, size_t b, void *u) {
  size_t n = fwrite(p, a, b, (FILE *)u) * a;
  g_dlGot += (long long)n;
  return n;
}
static int curlProgress(void *, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) {
  if (total > 0) g_dlTotal = (long long)total;
  return 0;
}
static bool download(const std::string &url, FILE *f, std::string *err) {
  CURL *c = curl_easy_init();
  if (!c) {
    *err = "no connection";
    return false;
  }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, "OLAAF/" OLAAF_VERSION_STRING);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); // github.com sends the file from its download servers
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L); // stalled for 30 s: give up
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlFile);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlProgress);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) *err = curl_easy_strerror(rc);
  else if (code != 200) *err = "GitHub answered " + std::to_string(code);
  return rc == CURLE_OK && code == 200;
}
#else
static bool download(const std::string &, FILE *, std::string *err) {
  *err = "downloads are not available here";
  return false;
}
#endif
// runs a program and waits for it (no shell: every argument stays one argument)
[[maybe_unused]] static int run(std::vector<std::string> args) {
  std::vector<char *> argv;
  for (auto &a : args) argv.push_back((char *)a.c_str());
  argv.push_back(NULL);
  pid_t pid;
  if (posix_spawn(&pid, argv[0], NULL, NULL, argv.data(), environ) != 0) return -1;
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif

void openInstaller(const std::string &path) {
  if (path.empty()) return;
#ifdef _WIN32
  ShellExecuteW(NULL, L"open", wide(path).c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::thread([path]() {
    // mount the disk image out of sight and open its installer right away; if that does not work, the
    // disk image's window (with the three steps) the usual way
    std::string mnt = "/tmp/olaaf-update";
    run({"/usr/bin/hdiutil", "detach", "-quiet", mnt});
    run({"/bin/mkdir", "-p", mnt});
    if (run({"/usr/bin/hdiutil", "attach", "-nobrowse", "-noautoopen", "-quiet", "-mountpoint", mnt, path}) == 0 &&
        run({"/usr/bin/open", mnt + "/Install OLAAF.pkg"}) == 0)
      return;
    run({"/usr/bin/open", path});
  }).detach();
#else
  revealInFileBrowser(path);
#endif
}

void updateDownloadStart(bool install) {
  int idle = 0, failed = 3;
  if (!g_dl.compare_exchange_strong(idle, 1) && !g_dl.compare_exchange_strong(failed, 1)) return; // one at a time
  std::string url = updateAssetUrl(), dir = updateFolder();
  g_dlGot = 0;
  g_dlTotal = 0;
  std::thread([url, dir, install]() {
    std::string err, path;
    if (url.compare(0, strlen(kAssetBase), kAssetBase) != 0) err = "no installer for this system";
    else if (dir.empty()) err = "no download folder";
    else {
      path = dir + "/" + url.substr(url.find_last_of('/') + 1);
      std::string part = path + ".part";
      FILE *f = openOut(part);
      if (!f) err = "could not write to " + dir;
      else {
        bool ok = download(url, f, &err);
        if (fclose(f) != 0 && ok) {
          ok = false;
          err = "could not write the file";
        }
        if (ok && g_dlTotal > 0 && g_dlGot != g_dlTotal) {
          ok = false;
          err = "the download is incomplete";
        }
        if (ok && !moveOver(part, path)) {
          ok = false;
          err = "could not write " + path;
        }
        if (!ok) removeFile(part);
      }
    }
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_dlPath = err.empty() ? path : std::string();
      g_dlError = err;
    }
    g_dl = err.empty() ? 2 : 3;
    if (err.empty() && install) openInstaller(path);
  }).detach();
}

int updateDownloadState(double *progress, std::string *path, std::string *error) {
  int s = g_dl.load();
  if (progress) {
    long long t = g_dlTotal.load();
    *progress = t > 0 ? std::min(1.0, (double)g_dlGot.load() / (double)t) : -1;
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (path) *path = g_dlPath;
  if (error) *error = g_dlError;
  return s;
}

void openWebPage(const std::string &url) {
  if (url.compare(0, 19, "https://github.com/") != 0) return;
  for (char ch : url)
    if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '`') return;
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, NULL, 0);
  std::vector<wchar_t> w((size_t)n);
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
  ShellExecuteW(NULL, L"open", w.data(), NULL, NULL, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
  const char *tool = "/usr/bin/open";
#else
  const char *tool = "xdg-open";
#endif
  pid_t pid;
  char *argv[] = {(char *)tool, (char *)url.c_str(), NULL};
  if (posix_spawnp(&pid, tool, NULL, NULL, argv, environ) == 0) {
    std::thread([pid]() {
      int st;
      waitpid(pid, &st, 0);
    }).detach();
  }
#endif
}

void revealInFileBrowser(const std::string &path) {
  if (path.empty()) return;
#ifdef _WIN32
  std::string q = "/select,\"";
  for (char c : path) q += c == '/' ? '\\' : c;
  q += "\"";
  int n = MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, NULL, 0);
  std::vector<wchar_t> w((size_t)n);
  MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, w.data(), n);
  ShellExecuteW(NULL, L"open", L"explorer.exe", w.data(), NULL, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
  const char *tool = "/usr/bin/open";
  char *argv[] = {(char *)tool, (char *)"-R", (char *)path.c_str(), NULL};
#else
  const char *tool = "xdg-open";
  std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
  char *argv[] = {(char *)tool, (char *)dir.c_str(), NULL};
#endif
  pid_t pid; // no shell: the path is one argument, whatever it holds
  if (posix_spawnp(&pid, tool, NULL, NULL, argv, environ) == 0) {
    std::thread([pid]() {
      int st;
      waitpid(pid, &st, 0);
    }).detach();
  }
#endif
}

} // namespace olaaf
