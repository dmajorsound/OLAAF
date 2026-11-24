// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "update.h"

#include <atomic>
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
#include <sys/wait.h>
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
static std::string g_tag, g_url;

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
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_tag = tag;
      g_url = url;
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
