// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include <cstdlib>
#include "importer.h"
#include "audiofile.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <set>

namespace olaaf {

// lower case for matching names, with the German capitals too ("GERÄUSCHE" = "geräusche")
static std::string foldCase(const std::string &in) {
  std::string s = lower(in);
  for (size_t i = 0; i + 1 < s.size(); i++)
    if ((unsigned char)s[i] == 0xC3) {
      unsigned char &b = (unsigned char &)s[i + 1];
      if (b == 0x84 || b == 0x96 || b == 0x9C) b = (unsigned char)(b + 0x20); // Ä Ö Ü -> ä ö ü
    }
  return s;
}

static std::vector<std::string> tokens(const std::string &name) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : foldCase(name)) {
    if (std::isalnum(c) || c >= 0x80) cur += (char)c;
    else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// The sort rules: folder name, color and the words that send a track there (first rule wins).
static std::vector<SortRule> g_rules = defaultSortRules();

std::vector<SortRule> defaultSortRules() {
  return {
      {"DX", 0xE8923A, {"dia", "dx", "dlg", "dial", "dialog", "dialogue", "boom", "lav", "anstecker", "fr", "vo", "adr", "voice",
                         "ot", "o-ton", "oton", "guide", "sprache", "mic", "off", "narration", "narr", "voice over"}},
      {"FX", 0xF2A6C8, {"sfx", "fx", "effect", "effects", "hardfx", "design", "sd", "foley", "fol", "ger\xc3\xa4usche",
                        "ger\xc3\xa4usch", "gerausche", "steps", "fs", "spot", "moves", "zs", "prop", "nt", "wildtrack"}},
      {"MX", 0x7DB4F0, {"mus", "music", "musik", "mx", "score", "song", "source", "temp", "layout"}},
      {"BG", 0x8FD48A, {"atmo", "atmos", "amb", "ambi", "ambience", "bg", "bgs", "roomtone", "rt", "at", "raum"}},
  };
}
// Folders saved by older OLAAF versions (DIA, MUS, ATMO and their old colors) become today's DX, FX, MX, BG.
// A color the user picked stays; only the old default colors are replaced. Returns true when something changed.
bool migrateFolder(std::string &name, uint32_t &color) {
  static const std::pair<const char *, const char *> oldNames[] = {{"DIA", "DX"}, {"MUS", "MX"}, {"ATMO", "BG"}};
  static const uint32_t oldColors[] = {0x5580d4, 0xd9822b, 0xc8423b, 0x1f9a8a, 0x4a86e8};
  bool changed = false;
  for (auto &o : oldNames)
    if (foldCase(name) == foldCase(o.first)) { name = o.second; changed = true; }
  bool oldColor = std::find(std::begin(oldColors), std::end(oldColors), color & 0xffffff) != std::end(oldColors);
  if (oldColor)
    for (auto &d : defaultSortRules())
      if (d.name == name) { color = d.color; changed = true; }
  return changed;
}

void setSortRules(const std::vector<SortRule> &rules) { g_rules = rules.empty() ? defaultSortRules() : rules; }
const std::vector<SortRule> &sortRules() { return g_rules; }

// "DX|e8923a|boom,lav,...;FX|d9822b|..." (for REAPER's settings)
std::string sortRulesToText(const std::vector<SortRule> &rules) {
  std::string out;
  for (auto &r : rules) {
    if (!out.empty()) out += ";";
    out += r.name + "|" + strprintf("%06x", r.color & 0xffffff) + "|";
    for (size_t i = 0; i < r.words.size(); i++) out += (i ? "," : "") + r.words[i];
  }
  return out;
}

std::vector<SortRule> sortRulesFromText(const std::string &text) {
  std::vector<SortRule> out;
  size_t a = 0;
  while (a < text.size()) {
    size_t e = text.find(';', a);
    if (e == std::string::npos) e = text.size();
    std::string row = text.substr(a, e - a);
    size_t p1 = row.find('|'), p2 = p1 == std::string::npos ? p1 : row.find('|', p1 + 1);
    if (p2 != std::string::npos) {
      SortRule r;
      r.name = row.substr(0, p1);
      r.color = (uint32_t)strtoul(row.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 16);
      r.words = splitWords(row.substr(p2 + 1));
      if (migrateFolder(r.name, r.color)) // an older version's folder: it also gets today's words
        for (auto &d : defaultSortRules())
          if (d.name == r.name)
            for (auto &w : d.words)
              if (std::find(r.words.begin(), r.words.end(), w) == r.words.end()) r.words.push_back(w);
      if (!r.name.empty()) out.push_back(r);
    }
    a = e + 1;
  }
  return out;
}

// "boom, lav,  dia" -> {"boom", "lav", "dia"} (lower case, no empty ones)
std::vector<std::string> splitWords(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i <= text.size(); i++) {
    char c = i < text.size() ? text[i] : ',';
    if (c == ',' || c == ';') {
      size_t b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
      if (b != std::string::npos) out.push_back(lower(cur.substr(b, e - b + 1)));
      cur.clear();
    } else
      cur += c;
  }
  return out;
}

std::string trackGroup(const std::string &trackName) {
  auto tk = tokens(trackName);
  std::string low = foldCase(trackName); // never case sensitive
  for (auto &row : g_rules) // words with a separator ("o-ton"): anywhere in the name
    for (auto &k : row.words)
      if (k.find_first_of(" -_.") != std::string::npos && low.find(foldCase(k)) != std::string::npos) return row.name;
  // whole words first (so "FX Boom" is not caught by a longer word elsewhere), then word starts
  for (int pass = 0; pass < 2; pass++)
    for (auto &row : g_rules)
      for (auto &t : tk)
        for (auto &k : row.words) {
          std::string kl = foldCase(k);
          // a number right after the word still counts: "dx1", "FX12", "BG_2"
          bool whole = t == kl || (t.size() > kl.size() && t.compare(0, kl.size(), kl) == 0 &&
                                   t.find_first_not_of("0123456789", kl.size()) == std::string::npos);
          if (pass == 0 ? whole : (kl.size() >= 4 && t.compare(0, kl.size(), kl) == 0)) return row.name;
        }
  return "OTHER";
}

uint32_t groupColor(const std::string &g) {
  for (auto &r : g_rules)
    if (r.name == g) return r.color;
  return 0x6B7480;
}

Folder *Session::folder(const std::string &key) {
  for (auto &f : folders)
    if (f.key == key) return &f;
  return nullptr;
}


void Importer::assignDefaultFolders() {
  std::set<std::string> present;
  for (auto &t : s_.tracks) present.insert(trackGroup(t.name));
  s_.folders.clear();
  if (present.size() < 2) { // one folder for everything does not help
    for (auto &t : s_.tracks) t.group.clear();
    return;
  }
  for (auto &r : g_rules)
    if (present.count(r.name)) s_.folders.push_back({r.name, r.name, r.color});
  if (present.count("OTHER")) s_.folders.push_back({"OTHER", "OTHER", groupColor("OTHER")});
  for (auto &t : s_.tracks) t.group = trackGroup(t.name);
}

void Importer::clearFolders() {
  s_.folders.clear();
  for (auto &t : s_.tracks) t.group.clear();
  // back in the AAF's order (uids count up in it; tracks made here come last)
  std::stable_sort(s_.tracks.begin(), s_.tracks.end(), [](const Track &a, const Track &b) { return a.uid < b.uid; });
}

std::string Importer::addFolder(const std::string &name) {
  static const uint32_t pal[] = {0x5580d4, 0x1F9A8A, 0x4E8F3A, 0xB8962E, 0xD9822B, 0xC8423B, 0xB0457E, 0x7A5AC8};
  int n = 1;
  while (s_.folder("F" + std::to_string(n))) n++;
  std::string key = "F" + std::to_string(n);
  s_.folders.push_back({key, name, pal[(s_.folders.size()) % 8]});
  return key;
}

void Importer::removeFolder(const std::string &key) {
  for (size_t i = 0; i < s_.folders.size(); i++)
    if (s_.folders[i].key == key) {
      s_.folders.erase(s_.folders.begin() + (long)i);
      break;
    }
  for (auto &t : s_.tracks)
    if (t.group == key) t.group.clear();
}

std::string embeddedCacheName(const std::string &aafPath) {
  uint64_t size = 0, h = 1469598103934665603ull;
  int64_t changed = 0;
  fileStamp(aafPath, &size, &changed);
  for (unsigned char c : lower(aafPath) + strprintf("|%llu|%lld", (unsigned long long)size, (long long)changed))
    h = (h ^ c) * 1099511628211ull;
  return strprintf("%016llx", (unsigned long long)h);
}

int Importer::embeddedCount(uint64_t *bytes) const {
  int n = 0;
  uint64_t b = 0;
  for (auto &m : s_.media)
    if (m.kind == MediaKind::Embedded && m.foundPath.empty()) {
      n++;
      b += (uint64_t)std::max<int64_t>(0, m.length) * (uint64_t)std::max(1, m.channels) * (uint64_t)(std::max(8, m.bits) / 8);
    }
  if (bytes) *bytes = b;
  return n;
}

std::string Importer::extractEmbeddedPreview(int media, const std::string &dir) {
  if (!aaf_ || media < 0 || media >= (int)s_.media.size() || !makeDirs(dir)) return "";
  return aaf_->extractEmbedded(s_.media[(size_t)media], dir);
}

// index.txt: one line per embedded file, "media index <tab> file name"
bool Importer::loadEmbeddedCache(const std::string &dir) {
  FILE *f = openUtf8(joinPath(dir, "index.txt"), "rb");
  if (!f) return false;
  std::map<int, std::string> got;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    std::string name = tab + 1;
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    got[atoi(line)] = joinPath(dir, name);
  }
  fclose(f);
  for (size_t i = 0; i < s_.media.size(); i++)
    if (s_.media[i].kind == MediaKind::Embedded && (!got.count((int)i) || !fileExists(got[(int)i]))) return false;
  for (size_t i = 0; i < s_.media.size(); i++)
    if (s_.media[i].kind == MediaKind::Embedded) s_.media[i].foundPath = got[(int)i];
  return true;
}

void Importer::saveEmbeddedCache(const std::string &dir) const {
  std::string text;
  for (size_t i = 0; i < s_.media.size(); i++) {
    if (s_.media[i].kind != MediaKind::Embedded) continue;
    if (s_.media[i].foundPath.empty()) return; // not complete: no index
    text += strprintf("%d\t", (int)i) + baseName(s_.media[i].foundPath) + "\n";
  }
  if (text.empty()) return;
  if (FILE *f = openUtf8(joinPath(dir, "index.txt"), "wb")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
  }
}

std::string defaultSubdir(const Session &s) {
  return safeFileName((s.name.empty() ? stemName(s.aafPath) : s.name) + "_Audio");
}

bool Importer::load(const std::string &aafPath, const ImportOptions &opt, std::string *error) {
  s_ = Session();
  unmerged_.clear();
  haveUnmerged_ = false;
  aaf_.reset(new AafFile());
  if (!aaf_->load(aafPath, opt.read, &s_, error)) return false;
  int id = 1;
  for (auto &t : s_.tracks) {
    t.aafName = t.name;
    for (auto &c : t.clips) c.id = id++;
  }
  // no sorting on its own: the tracks come in as the AAF has them ("Sort" in the dialog sorts them)
  assignUids();
  return true;
}

// ------------------------------------------------------------------ locate

void Importer::locateMedia(const ImportOptions &opt) {
  std::vector<std::pair<std::string, std::string>> index;
  // where to look, in this order (the first match of a name wins):
  // the folders the user pointed to, the AAF's folder, then the folder around it (an AAF often sits
  // in its own subfolder next to "Audio Files" or "Media"), each with all their subfolders
  for (auto &f : opt.searchFolders) indexFolder(f, 12, &index);
  std::string aafDir = dirName(s_.aafPath);
  indexFolder(aafDir, 8, &index);
  std::string parent = dirName(aafDir);
  auto broad = [](const std::string &d) { // too wide to walk: a disk, /Users, a home folder
    if (d.empty() || d == "/" || d == "/Volumes" || d == "/Users" || d == "/home") return true;
    if (d.size() <= 3 && d.size() >= 2 && d[1] == ':') return true; // C:\ ...
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (home) { // the home folder itself and its big standard folders
      std::string h = home;
      for (const char *sub : {"", "/Desktop", "/Documents", "/Downloads", "/Music", "/Movies", "/Pictures", "/Videos"})
        if (d == h + sub || d == h + sub + "/") return true;
    }
    int depth = 0;
    for (char c : d) depth += c == '/' || c == '\\';
    return depth < 2;
  };
  if (!broad(parent) && parent != aafDir) indexFolder(parent, 6, &index, 150000);
  // exact (case-insensitive) name, then a loose name where spaces, _ and - count as equal
  // (files copied through some tools or uploads lose their spaces)
  auto loose = [](const std::string &n) {
    std::string o;
    for (char c : n) o += (c == ' ' || c == '-' || c == '_') ? '_' : c;
    return o;
  };
  std::map<std::string, std::string> byName, byLoose;
  for (auto &e : index) {
    byName.emplace(e.first, e.second);
    byLoose.emplace(loose(e.first), e.second);
  }

  auto findFile = [&](const std::string &uri) -> std::string {
    std::string p = uriToPath(uri);
    std::vector<std::string> tries = {p};
    if (p.size() > 1 && p[0] == '/') {
      size_t s2 = p.find('/', 1);
      if (s2 != std::string::npos) {
        tries.push_back(p.substr(s2));
        tries.push_back("/Volumes" + p);
      }
    }
    for (auto &t : tries)
      if (fileExists(t)) return t;
    std::string b = lower(baseName(p));
    auto it = byName.find(b);
    if (it != byName.end()) return it->second;
    auto il = byLoose.find(loose(b));
    if (il != byLoose.end()) return il->second;
    return "";
  };
  for (auto &v : s_.video)
    if (!fileExists(v.foundPath)) v.foundPath = findFile(v.originalUri);

  for (auto &m : s_.media) {
    if (m.kind == MediaKind::Embedded) continue;
    if (fileExists(m.foundPath)) continue;
    m.foundPath.clear();
    std::string p = uriToPath(m.originalUri);
    // macOS editors write the volume name first: "/Macintosh HD/Users/..." -> "/Users/...",
    // other disks live under /Volumes: "/Media SSD/Project/..." -> "/Volumes/Media SSD/Project/..."
    std::vector<std::string> tries = {p};
    if (p.size() > 1 && p[0] == '/') {
      size_t s2 = p.find('/', 1);
      if (s2 != std::string::npos) {
        tries.push_back(p.substr(s2));
        tries.push_back("/Volumes" + p);
      }
    }
    for (auto &t : tries)
      if (m.foundPath.empty() && fileExists(t)) m.foundPath = t;
    if (m.foundPath.empty()) {
      std::string b = lower(baseName(p));
      auto it = byName.find(b);
      if (it != byName.end()) m.foundPath = it->second;
      else {
        auto il = byLoose.find(loose(b));
        if (il != byLoose.end()) m.foundPath = il->second;
      }
    }
    if (!m.foundPath.empty() && m.kind == MediaKind::Unknown) {
      std::string e = lower(fileExt(m.foundPath));
      m.kind = e == "mxf" ? MediaKind::Mxf : (e == "aif" || e == "aiff" || e == "aifc") ? MediaKind::Aiff : MediaKind::Wav;
    }
  }
}

// ------------------------------------------------------------------ stereo merge

static bool splitSide(const std::string &name, std::string *base, char *side) {
  // "SFX 1.L", "SFX 1.R", "SFX 1_L", "SFX 1 L", "SFX 1.Left"
  static const char *suf[][2] = {{".L", "L"}, {".R", "R"}, {"_L", "L"}, {"_R", "R"}, {" L", "L"},
                                 {" R", "R"}, {".Left", "L"}, {".Right", "R"}, {".l", "L"}, {".r", "R"}};
  for (auto &s : suf) {
    size_t n = strlen(s[0]);
    if (name.size() > n && name.compare(name.size() - n, n, s[0]) == 0) {
      *base = name.substr(0, name.size() - n);
      *side = s[1][0];
      return true;
    }
  }
  return false;
}

static bool sameTimeline(const Track &a, const Track &b) {
  if (a.clips.size() != b.clips.size() || a.clips.empty()) return false;
  const double eps = 0.5 / 48000.0;
  for (size_t i = 0; i < a.clips.size(); i++) {
    const Clip &x = a.clips[i], &y = b.clips[i];
    if (std::fabs(x.pos - y.pos) > eps || std::fabs(x.len - y.len) > eps || std::fabs(x.srcOffset - y.srcOffset) > eps)
      return false;
    if (x.sources.size() != 1 || y.sources.size() != 1) return false;
  }
  return true;
}

// ------------------------------------------------------------------ event edits

static double envAt(const std::vector<GainPoint> &env, double t, double def) {
  if (env.empty()) return def;
  if (t <= env.front().time) return env.front().gain;
  if (t >= env.back().time) return env.back().gain;
  for (size_t i = 1; i < env.size(); i++)
    if (t <= env[i].time) {
      const GainPoint &a = env[i - 1], &b = env[i];
      double f = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0;
      return a.gain + (b.gain - a.gain) * f;
    }
  return def;
}

// copy the source envelope over [t0, t1] into dst, keeping dst's own values outside
static void spliceEnv(const std::vector<GainPoint> &src, double srcDef, std::vector<GainPoint> &dst, double dstDef,
                      double t0, double t1) {
  if (src.empty() && dst.empty()) return;
  std::vector<GainPoint> out;
  const double eps = 1e-4;
  for (auto &p : dst)
    if (p.time < t0 - eps) out.push_back(p);
  out.push_back({t0 - eps, envAt(dst, t0, dstDef)});
  out.push_back({t0, envAt(src, t0, srcDef)});
  for (auto &p : src)
    if (p.time > t0 && p.time < t1) out.push_back(p);
  out.push_back({t1, envAt(src, t1, srcDef)});
  out.push_back({t1 + eps, envAt(dst, t1, dstDef)});
  for (auto &p : dst)
    if (p.time > t1 + eps) out.push_back(p);
  dst.swap(out);
}

Clip *Importer::findClip(int id, int *track) {
  for (size_t t = 0; t < s_.tracks.size(); t++)
    for (auto &c : s_.tracks[t].clips)
      if (c.id == id) {
        if (track) *track = (int)t;
        return &c;
      }
  return nullptr;
}

void Importer::setExcluded(const std::vector<int> &ids, bool ex) {
  for (int id : ids)
    if (Clip *c = findClip(id)) c->excluded = ex;
}

void Importer::moveClips(const std::vector<int> &ids, int target, bool withAutomation) {
  if (target < 0 || target >= (int)s_.tracks.size()) return;
  for (int id : ids) {
    int from = -1;
    Clip *c = findClip(id, &from);
    if (!c || from == target) continue;
    Clip moved = *c;
    Track &src = s_.tracks[(size_t)from];
    Track &dst = s_.tracks[(size_t)target];
    if (withAutomation) {
      double t0 = moved.pos, t1 = moved.pos + moved.len;
      if (!src.volEnv.empty() || !dst.volEnv.empty()) {
        // a target without automation still plays at its fader level outside the clip
        spliceEnv(src.volEnv, src.volume, dst.volEnv, dst.volume, t0, t1);
      }
      if (!src.panEnv.empty() || !dst.panEnv.empty()) spliceEnv(src.panEnv, src.pan, dst.panEnv, dst.pan, t0, t1);
    }
    src.clips.erase(src.clips.begin() + (c - src.clips.data()));
    auto at = std::lower_bound(dst.clips.begin(), dst.clips.end(), moved,
                               [](const Clip &a, const Clip &b) { return a.pos < b.pos; });
    dst.clips.insert(at, moved);
    if ((int)moved.sources.size() > dst.channels) dst.channels = (int)moved.sources.size();
  }
}

void Importer::assignUids() {
  int u = 1;
  for (auto &t : s_.tracks) {
    t.uid = u++;
    for (auto &c : t.clips) c.home = t.uid;
  }
}

bool Importer::removeCreatedTrack(int track, bool withAutomation) {
  if (track < 0 || track >= (int)s_.tracks.size() || !s_.tracks[(size_t)track].created) return false;
  std::vector<int> ids;
  for (auto &c : s_.tracks[(size_t)track].clips) ids.push_back(c.id);
  for (int id : ids) {
    Clip *c = findClip(id);
    if (!c) continue;
    int home = -1;
    for (size_t i = 0; i < s_.tracks.size(); i++)
      if (s_.tracks[i].uid == c->home && !s_.tracks[i].created) home = (int)i;
    if (home < 0) // its AAF track is gone (should not happen): leave it out instead of losing it silently
      for (size_t i = 0; i < s_.tracks.size(); i++)
        if (!s_.tracks[i].created) { home = (int)i; c->excluded = true; break; }
    if (home >= 0) moveClips({id}, home, withAutomation);
  }
  s_.tracks.erase(s_.tracks.begin() + track);
  return true;
}

int Importer::stereoPartner(int track) const {
  if (track < 0 || track >= (int)s_.tracks.size()) return -1;
  const Track &t = s_.tracks[(size_t)track];
  std::string base, b2;
  char side, s2;
  if (t.channels != 1 || !t.mergedFrom.empty() || !splitSide(t.name, &base, &side)) return -1;
  for (size_t j = 0; j < s_.tracks.size(); j++) {
    const Track &o = s_.tracks[j];
    if ((int)j != track && o.channels == 1 && splitSide(o.name, &b2, &s2) && b2 == base && s2 != side && s2 != 0 &&
        ((side == 'L' && s2 == 'R') || (side == 'R' && s2 == 'L')))
      return (int)j;
  }
  return -1;
}

int Importer::combineStereo(int track, std::string *why) {
  int p = stereoPartner(track);
  if (p < 0) {
    if (why) *why = "This track has no .L / .R partner.";
    return -1;
  }
  std::string base;
  char side;
  splitSide(s_.tracks[(size_t)track].name, &base, &side);
  int r = side == 'L' ? combineTracks(track, p, why) : combineTracks(p, track, why);
  if (r >= 0) s_.tracks[(size_t)r].name = s_.tracks[(size_t)r].aafName = base;
  return r;
}

std::string Importer::stereoName(const std::string &left, const std::string &right) {
  size_t n = 0;
  while (n < left.size() && n < right.size() && foldCase(left.substr(n, 1)) == foldCase(right.substr(n, 1))) n++;
  std::string stem = left.substr(0, n);
  // "Boom 1" / "Boom 2", "OH Left" / "OH Right", "Atmo_L_01": cut what only one side has
  while (!stem.empty() && strchr(" ._-()[]", stem.back())) stem.pop_back();
  for (const char *cut : {" L", " R", " Left", " Right", " Lt", " Rt"})
    if (stem.size() > strlen(cut) && foldCase(stem.substr(stem.size() - strlen(cut))) == foldCase(cut)) {
      stem.resize(stem.size() - strlen(cut));
      break;
    }
  while (!stem.empty() && strchr(" ._-()[]", stem.back())) stem.pop_back();
  return stem.size() >= 2 ? stem : left + " + " + right;
}

int Importer::combineTracks(int left, int right, std::string *why) {
  int n = (int)s_.tracks.size();
  if (left < 0 || right < 0 || left >= n || right >= n || left == right) {
    if (why) *why = "Select two tracks to combine.";
    return -1;
  }
  size_t i = (size_t)left, j = (size_t)right;
  for (size_t k : {i, j})
    if (s_.tracks[k].channels != 1 || !s_.tracks[k].mergedFrom.empty()) {
      if (why) *why = "\"" + s_.tracks[k].name + "\" isn't mono, so it can't be one side of a stereo track.";
      return -1;
    }
  if (!sameTimeline(s_.tracks[i], s_.tracks[j])) {
    if (why)
      *why = "\"" + s_.tracks[i].name + "\" and \"" + s_.tracks[j].name +
             "\" can't be combined: their events don't line up (one has events the other hasn't, or they were moved).";
    return -1;
  }
  Track &L = s_.tracks[i];
  Track &R = s_.tracks[j];
  for (auto &o : s_.tracks) // events that came from R now belong to the stereo track
    for (auto &c : o.clips)
      if (c.home == R.uid) c.home = L.uid;
  L.mergedFrom = L.name + " + " + R.name;
  L.panL = L.pan;
  L.panR = R.pan;
  L.pan = 0.0;
  L.panEnv.clear();
  L.name = stereoName(L.name, R.name);
  L.aafName = L.name;
  L.channels = 2;
  for (size_t c = 0; c < L.clips.size(); c++) {
    L.clips[c].sources.push_back(R.clips[c].sources[0]);
    L.clips[c].excluded = L.clips[c].excluded && R.clips[c].excluded; // removed only if both halves were
  }
  s_.tracks.erase(s_.tracks.begin() + (long)j);
  return (int)(j < i ? i - 1 : i);
}

int Importer::splitStereo(int track, std::string *why) {
  if (track < 0 || track >= (int)s_.tracks.size()) return -1;
  Track &M = s_.tracks[(size_t)track];
  size_t plus = M.mergedFrom.find(" + ");
  if (M.channels != 2 || plus == std::string::npos) {
    if (why) *why = "Only a track combined from .L / .R can be split.";
    return -1;
  }
  int maxId = 0, maxUid = 0;
  for (auto &t : s_.tracks) {
    maxUid = std::max(maxUid, t.uid);
    for (auto &c : t.clips) maxId = std::max(maxId, c.id);
  }
  Track R = M;
  R.name = M.mergedFrom.substr(plus + 3);
  R.aafName = R.name;
  R.uid = maxUid + 1;
  R.channels = 1;
  R.pan = M.panR;
  R.mergedFrom.clear();
  R.clips.clear();
  M.name = M.mergedFrom.substr(0, plus);
  M.aafName = M.name;
  M.channels = 1;
  M.pan = M.panL;
  M.mergedFrom.clear();
  std::vector<Clip> keepL;
  for (auto &c : M.clips) {
    if (c.sources.size() == 2) { // left half stays, right half goes to R (exchanged: the other way round)
      Clip r = c;
      r.id = ++maxId;
      r.home = c.home == M.uid ? R.uid : c.home;
      r.sources = {c.sources[c.swapLR ? 0 : 1]};
      r.swapLR = false;
      R.clips.push_back(r);
      Clip l = c;
      l.sources = {c.sources[c.swapLR ? 1 : 0]};
      l.swapLR = false;
      keepL.push_back(l);
    } else
      keepL.push_back(c); // an event moved here that was not a pair stays on the L track
  }
  M.clips = keepL;
  s_.tracks.insert(s_.tracks.begin() + track + 1, R);
  return track;
}

bool Importer::trackIsEmpty(int track) const {
  if (track < 0 || track >= (int)s_.tracks.size()) return false;
  const Track &t = s_.tracks[(size_t)track];
  if (!t.clips.empty() || !t.volEnv.empty() || !t.panEnv.empty()) return false;
  for (auto &o : s_.tracks) // an event moved away from it still belongs to it
    for (auto &c : o.clips)
      if (c.home == t.uid) return false;
  return true;
}

std::vector<int> Importer::emptyTracks() const {
  std::vector<int> out;
  for (size_t i = 0; i < s_.tracks.size(); i++)
    if (trackIsEmpty((int)i)) out.push_back((int)i);
  return out;
}

int Importer::removeEmptyTracks(std::vector<std::string> *names) {
  std::vector<std::string> own;
  if (!names) names = &own;
  int n = 0;
  for (int i = (int)s_.tracks.size() - 1; i >= 0; i--) // from the end: the indexes stay right
    if (trackIsEmpty(i)) {
      if (names) names->insert(names->begin(), s_.tracks[(size_t)i].name);
      s_.tracks.erase(s_.tracks.begin() + i);
      n++;
    }
  // the copy kept for the .L / .R setting: the same (empty) tracks go there too, by name
  if (n && haveUnmerged_ && names)
    for (int i = (int)unmerged_.size() - 1; i >= 0; i--) {
      const Track &t = unmerged_[(size_t)i];
      bool gone = std::find(names->begin(), names->end(), t.name) != names->end();
      if (gone && t.clips.empty() && t.volEnv.empty() && t.panEnv.empty()) unmerged_.erase(unmerged_.begin() + i);
    }
  return n;
}

int Importer::addTrack(int like) {
  Track t;
  int n = 1;
  for (auto &x : s_.tracks)
    if (x.created) n++;
  t.name = strprintf("New track %d", n);
  t.created = true;
  int maxUid = 0;
  for (auto &x : s_.tracks) maxUid = std::max(maxUid, x.uid);
  t.uid = maxUid + 1;
  if (like >= 0 && like < (int)s_.tracks.size()) {
    t.channels = s_.tracks[(size_t)like].channels;
    t.group = s_.tracks[(size_t)like].group;
  }
  s_.tracks.push_back(t);
  return (int)s_.tracks.size() - 1;
}

void Importer::reorderTracks(const std::vector<int> &newOrder) {
  if (newOrder.size() != s_.tracks.size()) return;
  std::vector<Track> t;
  t.reserve(newOrder.size());
  for (int i : newOrder) t.push_back(std::move(s_.tracks[(size_t)i]));
  s_.tracks = std::move(t);
}

// clip gain (constant x clip automation) of the clip that plays at t, 1 where nothing plays
static double clipGainAt(const Track &t, double time) {
  for (auto &c : t.clips) {
    if (c.excluded || time < c.pos || time > c.pos + c.len) continue;
    return c.gain * (c.gainEnv.empty() ? 1.0 : envAt(c.gainEnv, time - c.pos, 1.0));
  }
  return 1.0;
}

double Importer::trackVolumeAt(const Track &t, double time, const ImportOptions &opt) const {
  double v = t.volEnv.empty() ? t.volume : envAt(t.volEnv, time, t.volume);
  if (opt.clipGain && opt.clipGainToTrack) v *= clipGainAt(t, time);
  return v;
}

std::vector<GainPoint> Importer::trackVolumeEnvelope(const Track &t, const ImportOptions &opt) const {
  if (!(opt.clipGain && opt.clipGainToTrack)) return t.volEnv;
  // every place where the volume changes: the track's own points, each clip's edges and its gain points
  bool any = !t.volEnv.empty();
  std::vector<double> times;
  for (auto &p : t.volEnv) times.push_back(p.time);
  const double e = 0.0005; // a hard step at a clip edge (half a millisecond)
  for (auto &c : t.clips) {
    if (c.excluded) continue;
    if (std::fabs(c.gain - 1.0) > 1e-6 || !c.gainEnv.empty()) any = true;
    times.push_back(c.pos - e);
    times.push_back(c.pos);
    for (auto &p : c.gainEnv) times.push_back(c.pos + p.time);
    times.push_back(c.pos + c.len);
    times.push_back(c.pos + c.len + e);
  }
  std::vector<GainPoint> out;
  if (!any) return out;
  std::sort(times.begin(), times.end());
  double last = -1e18;
  for (double tm : times) {
    if (tm - last < 1e-7) continue;
    last = tm;
    double v = trackVolumeAt(t, tm, opt);
    if (out.size() >= 2 && std::fabs(out.back().gain - v) < 1e-9 && std::fabs(out[out.size() - 2].gain - v) < 1e-9)
      out.back().time = tm; // the same value three times: keep the line, drop the middle point
    else
      out.push_back({tm, v});
  }
  return out;
}

uint32_t Importer::trackColor(const Track &t, const ImportOptions &opt) const {
  if (t.color) return t.color;
  if (opt.folders && !t.group.empty())
    for (auto &f : s_.folders)
      if (f.key == t.group) return f.color;
  return 0;
}

int Importer::setMergeStereo(bool on) {
  if (!haveUnmerged_) {
    unmerged_ = s_.tracks;
    haveUnmerged_ = true;
  }
  s_.tracks = unmerged_;
  int n = on ? mergeStereoTracks() : 0;
  assignUids();
  // keep the folders as the user set them up; tracks go back to their folder by name
  for (auto &t : s_.tracks) t.group = s_.folder(trackGroup(t.name)) ? trackGroup(t.name) : std::string();
  return n;
}

int Importer::mergeStereoTracks() {
  int merged = 0;
  for (size_t i = 0; i < s_.tracks.size(); i++) {
    std::string bl;
    char sl;
    if (!splitSide(s_.tracks[i].name, &bl, &sl) || sl != 'L' || s_.tracks[i].channels != 1) continue;
    for (size_t j = 0; j < s_.tracks.size(); j++) {
      std::string br;
      char sr;
      if (j == i || !splitSide(s_.tracks[j].name, &br, &sr) || sr != 'R' || br != bl || s_.tracks[j].channels != 1)
        continue;
      if (!sameTimeline(s_.tracks[i], s_.tracks[j])) {
        s_.warnings.push_back("\"" + s_.tracks[i].name + "\" and \"" + s_.tracks[j].name +
                              "\" were not merged: their clips do not line up.");
        break;
      }
      Track &L = s_.tracks[i];
      Track &R = s_.tracks[j];
      L.mergedFrom = L.name + " + " + R.name;
      L.panL = L.pan;
      L.panR = R.pan;
      L.pan = 0.0; // the hard L / hard R pan of the halves is now the stereo file itself
      if (!L.panEnv.empty() || !R.panEnv.empty())
        s_.warnings.push_back("\"" + bl + "\": pan automation of the L/R tracks is not carried over to the stereo track.");
      L.panEnv.clear();
      L.name = bl;
      L.aafName = bl;
      L.channels = 2;
      for (size_t c = 0; c < L.clips.size(); c++) {
        L.clips[c].sources.push_back(R.clips[c].sources[0]);
        if (std::fabs(L.clips[c].gain - R.clips[c].gain) > 1e-6)
          s_.warnings.push_back("\"" + bl + "\": clip gain differs between L and R at " +
                                formatTC(L.clips[c].pos, s_.fps) + ", the L value is used.");
      }
      s_.tracks.erase(s_.tracks.begin() + (long)j);
      if (j < i) i--;
      merged++;
      break;
    }
  }
  return merged;
}

// ------------------------------------------------------------------ media

bool Importer::prepareMedia(const ImportOptions &opt, MediaReport *rep, const Progress &progress) {
  MediaReport r;
  std::string dest = opt.destDir.empty() ? dirName(s_.aafPath) : opt.destDir;
  std::string sub = opt.subdir.empty() ? defaultSubdir(s_) : safeFileName(opt.subdir);
  std::string target = opt.copyToSubdir ? joinPath(dest, sub) : dest;
  bool madeDir = false;
  auto ensureDir = [&]() {
    if (!madeDir) madeDir = makeDirs(target);
    return madeDir;
  };

  // which media are used by imported tracks
  std::set<int> used;
  for (auto &t : s_.tracks)
    if (t.import)
      for (auto &c : t.clips)
        if (!c.excluded)
          for (auto &src : c.sources)
            if (src.media >= 0) used.insert(src.media);

  size_t step = 0, total = used.size() + 1;
  for (int mi : used) {
    Media &m = s_.media[(size_t)mi];
    if (progress) {
      const char *verb = m.kind == MediaKind::Embedded ? "Extracting"
                       : (m.kind == MediaKind::Mxf || m.kind == MediaKind::Aiff) ? "Converting"
                       : opt.copyToSubdir ? "Copying" : "Linking";
      std::string fn = m.kind == MediaKind::Embedded || m.foundPath.empty() ? m.name : baseName(m.foundPath);
      if (!progress((double)step / (double)total,
                    strprintf("%s %s  \xc2\xb7  %d / %d", verb, fn.c_str(), (int)step + 1, (int)used.size())))
        return false;
    }
    step++;
    std::string err;
    std::string desc = m.name;

    if (m.kind == MediaKind::Embedded) {
      if (!ensureDir()) { s_.warnings.push_back("Cannot create folder " + target); continue; }
      if (fileExists(m.foundPath)) { // already unpacked for the dialog's waveforms: that file is copied
        std::string out = joinPath(target, baseName(m.foundPath));
        if (fileExists(out) || copyFile(m.foundPath, out)) {
          m.outPath = out;
          r.extracted++;
          continue;
        }
      }
      m.outPath = aaf_->extractEmbedded(m, target);
      if (m.outPath.empty()) s_.warnings.push_back("Could not extract embedded audio \"" + m.name + "\".");
      else r.extracted++;
      continue;
    }
    if (!fileExists(m.foundPath)) {
      r.missing++;
      r.missingNames.push_back(m.name);
      // keep the original location so REAPER shows the item offline and it can be relinked
      m.outPath = uriToPath(m.originalUri);
      continue;
    }
    r.found++;
    PcmInfo info;
    if (m.kind == MediaKind::Mxf || m.kind == MediaKind::Aiff) {
      bool ok = m.kind == MediaKind::Mxf ? readMxfInfo(m.foundPath, &info) : readAiffInfo(m.foundPath, &info);
      if (!ok) {
        s_.warnings.push_back("\"" + baseName(m.foundPath) + "\": " + info.error + ".");
        m.outPath = m.foundPath;
        continue;
      }
      if (!ensureDir()) { s_.warnings.push_back("Cannot create folder " + target); continue; }
      std::string out = uniquePath(joinPath(target, safeFileName(stemName(m.foundPath)) + ".wav"));
      int64_t tref = info.timeReference >= 0 ? info.timeReference : m.timeReference;
      if (convertToBwf(m.foundPath, info, out, tref, desc, &err)) {
        m.outPath = out;
        r.converted++;
      } else {
        s_.warnings.push_back(err);
        m.outPath = m.foundPath;
      }
    } else { // WAV stays WAV
      if (opt.copyToSubdir) {
        if (!ensureDir()) { s_.warnings.push_back("Cannot create folder " + target); continue; }
        std::string out = joinPath(target, baseName(m.foundPath));
        // the same source may be listed twice; copy once
        if (fileExists(out) || copyFile(m.foundPath, out)) {
          m.outPath = out;
          r.copied++;
        } else {
          s_.warnings.push_back("Could not copy " + m.foundPath);
          m.outPath = m.foundPath;
        }
      } else
        m.outPath = m.foundPath;
    }
  }

  // clips -> file. Multichannel clips built from mono files get one interleaved BWF.
  std::map<std::vector<int>, std::string> interleaved;
  for (auto &t : s_.tracks) {
    if (!t.import) continue;
    for (auto &c : t.clips) {
      if (c.excluded) continue;
      c.chanMode = 0;
      if (c.sources.empty() || c.sources[0].media < 0) continue;
      // left and right exchanged: an L / R pair is interleaved the other way round (this is the
      // last step before the session is written, so the clip can be changed here)
      if (c.swapLR && c.sources.size() == 2) {
        std::swap(c.sources[0], c.sources[1]);
        c.swapLR = false;
      }
      const Media &m0 = s_.media[(size_t)c.sources[0].media];
      if (c.sources.size() == 1) {
        c.outPath = m0.outPath;
        int ch = c.sources[0].channel;
        if (ch > 0 && m0.channels == 2) c.chanMode = ch == 1 ? 3 : 4;
        else if (ch > 0 && m0.channels > 2) c.chanMode = 2 + ch; // REAPER: 3+ = mono channel n-2
        else if (ch == 0 && m0.channels == 2 && c.swapLR) c.chanMode = 1; // REAPER: reverse stereo
        continue;
      }
      // several sources
      std::vector<int> key;
      bool allMono = true, allThere = true;
      for (auto &src : c.sources) {
        key.push_back(src.media);
        if (src.media < 0) { allThere = false; break; }
        const Media &m = s_.media[(size_t)src.media];
        if (m.channels != 1) allMono = false;
        if (!fileExists(m.outPath)) allThere = false;
      }
      // all channels of one multichannel file
      bool sameFile = true;
      for (auto &src : c.sources) sameFile = sameFile && src.media == c.sources[0].media;
      if (sameFile) { c.outPath = m0.outPath; continue; }

      auto it = interleaved.find(key);
      if (it != interleaved.end()) { c.outPath = it->second; continue; }
      if (!allMono || !allThere) {
        if (!allThere) {
          // offline: point at the first channel so the item is visible and relinkable
          c.outPath = m0.outPath;
        } else {
          s_.warnings.push_back("\"" + c.name + "\": channels from multichannel files are not combined yet, only the first is used.");
          c.outPath = m0.outPath;
        }
        continue;
      }
      std::vector<std::string> srcs;
      std::vector<PcmInfo> infos;
      bool ok = true;
      for (auto &src : c.sources) {
        PcmInfo pi;
        const std::string &p = s_.media[(size_t)src.media].outPath;
        if (!readWavInfo(p, &pi)) { ok = false; break; }
        srcs.push_back(p);
        infos.push_back(pi);
      }
      if (!ok || !ensureDir()) { c.outPath = m0.outPath; continue; }
      std::string base = safeFileName(stemName(m0.outPath));
      std::string out = uniquePath(joinPath(target, base + "_" + std::to_string(c.sources.size()) + "ch.wav"));
      int64_t tref = infos[0].timeReference >= 0 ? infos[0].timeReference : m0.timeReference;
      std::string err;
      if (progress && !progress((double)step / (double)total, "Combining " + base + " to " + std::to_string(c.sources.size()) + " channels")) return false;
      if (interleaveToBwf(srcs, infos, out, tref, m0.name, &err)) {
        interleaved[key] = out;
        c.outPath = out;
        r.interleaved++;
      } else {
        s_.warnings.push_back(err);
        c.outPath = m0.outPath;
      }
    }
  }
  // files olAAF wrote that no clip uses any more (e.g. mono halves of a combined stereo file)
  std::set<std::string> usedFiles;
  for (auto &t : s_.tracks)
    if (t.import)
      for (auto &c : t.clips) usedFiles.insert(c.outPath);
  for (int mi : used) {
    const Media &m = s_.media[(size_t)mi];
    bool written = m.kind == MediaKind::Embedded || m.kind == MediaKind::Mxf || m.kind == MediaKind::Aiff ||
                   (opt.copyToSubdir && m.outPath != m.foundPath);
    if (written && !m.outPath.empty() && !usedFiles.count(m.outPath) && m.outPath != m.foundPath &&
        fileExists(m.outPath))
      std::remove(m.outPath.c_str());
  }

  // a plain text log next to the audio, so problems can be traced on any machine
  if (ensureDir()) {
    r.logPath = joinPath(target, "OLAAF import log.txt");
    if (FILE *lf = openUtf8(r.logPath, "wb")) {
      fprintf(lf, "OLAAF import log\nAAF: %s\n%s %s %s\n%d Hz, %.3g fps, start %s\ndestination: %s\n\n", s_.aafPath.c_str(),
              s_.company.c_str(), s_.product.c_str(), s_.version.c_str(), s_.sampleRate, s_.fps,
              formatTC(s_.start, s_.fps).c_str(), target.c_str());
      for (int mi : used) {
        const Media &m = s_.media[(size_t)mi];
        const char *k = m.kind == MediaKind::Embedded ? "embedded" : m.kind == MediaKind::Mxf ? "mxf"
                      : m.kind == MediaKind::Aiff ? "aiff" : m.kind == MediaKind::Wav ? "wav" : "?";
        fprintf(lf, "[%s] %s\n  aaf:  %s\n  found: %s\n  used: %s%s\n", k, m.name.c_str(), m.originalUri.c_str(),
                m.foundPath.empty() ? "-" : m.foundPath.c_str(), m.outPath.c_str(),
                fileExists(m.outPath) ? "" : usedFiles.count(m.outPath) ? "   (MISSING)" : "   (combined into a multichannel file)");
      }
      fprintf(lf, "\ntracks:\n");
      for (auto &t : s_.tracks) {
        if (!t.import) continue;
        fprintf(lf, "  %s (%d ch, %zu clips)\n", t.name.c_str(), t.channels, t.clips.size());
        for (auto &c : t.clips)
          fprintf(lf, "    %s  %.3f s  %s%s\n", formatTC(c.pos, s_.fps).c_str(), c.len, c.outPath.c_str(),
                  fileExists(c.outPath) ? "" : "   (MISSING)");
      }
      if (!s_.warnings.empty()) fprintf(lf, "\nnotes:\n");
      for (auto &w : s_.warnings) fprintf(lf, "  %s\n", w.c_str());
      fclose(lf);
    }
  }

  if (progress) progress(1.0, "Done");
  if (rep) *rep = r;
  return true;
}

// ------------------------------------------------------------------ REAPER chunks

static std::string q(const std::string &s) {
  // RPP strings: use a quote character the text does not contain
  if (s.find('"') == std::string::npos) return "\"" + s + "\"";
  if (s.find('\'') == std::string::npos) return "'" + s + "'";
  std::string t = s;
  std::replace(t.begin(), t.end(), '`', '\'');
  return "`" + t + "`";
}

static std::string num(double v) { return strprintf("%.10f", v); }

static uint32_t nativeColor(uint32_t rgb) {
  // REAPER project files store colors as 0x01BBGGRR
  if (!rgb) return 0;
  uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
  return 0x1000000u | (b << 16) | (g << 8) | r;
}

// a track added in the dialog that nothing was moved onto
static bool emptyCreated(const Track &t) {
  if (!t.created) return false;
  for (auto &c : t.clips)
    if (!c.excluded) return false;
  return true;
}

std::vector<TrackChunk> Importer::trackChunks(const ImportOptions &opt) const {
  std::vector<TrackChunk> out;

  // video reference on top
  if (opt.video && !s_.video.empty()) {
    TrackChunk v;
    v.text = "<TRACK\nNAME \"VIDEO\"\nNCHAN 2\n";
    for (auto &vc : s_.video) {
      std::string f = vc.foundPath.empty() ? uriToPath(vc.originalUri) : vc.foundPath;
      double len = vc.len;
      if (vc.manual || len <= 0) { // length unknown here: the AAF's length for now, REAPER sets the file's own
        len = std::max(1.0, s_.end - vc.pos);
        v.videoFromFile = true;
      }
      v.text += "<ITEM\nPOSITION " + num(vc.pos + opt.tcOffset) + "\nLENGTH " + num(len) + "\nLOOP 0\n";
      v.text += "NAME " + q(vc.name.empty() ? baseName(f) : vc.name) + "\nSOFFS " + num(vc.srcOffset) + "\n";
      v.text += "<SOURCE VIDEO\nFILE " + q(f) + "\n>\n>\n";
    }
    v.text += ">\n";
    out.push_back(v);
  }

  // the order tracks are written in: folder by folder (as listed), then the tracks outside any folder
  std::vector<std::pair<std::string, const Track *>> order;
  if (opt.folders && !s_.folders.empty()) {
    for (auto &f : s_.folders)
      for (auto &t : s_.tracks)
        if (t.import && t.target.empty() && t.group == f.key && !emptyCreated(t)) order.push_back({f.key, &t});
    for (auto &t : s_.tracks)
      if (t.import && !emptyCreated(t) &&
          (!t.target.empty() || t.group.empty() || !const_cast<Session &>(s_).folder(t.group)))
        order.push_back({"", &t});
  } else
    for (auto &t : s_.tracks)
      if (t.import && !emptyCreated(t)) order.push_back({"", &t});
  for (size_t oi = 0; oi < order.size(); oi++) {
    const Track &t = *order[oi].second;
    const std::string &grp = order[oi].first;
    bool inFolder = !grp.empty();
    bool firstInGroup = inFolder && (oi == 0 || order[oi - 1].first != grp);
    bool lastInGroup = inFolder && (oi + 1 == order.size() || order[oi + 1].first != grp);
    if (firstInGroup) {
      const Folder *fd = const_cast<Session &>(s_).folder(grp);
      TrackChunk f;
      f.text = "<TRACK\nNAME " + q(fd->name) + "\n" + (fd->color ? strprintf("PEAKCOL %u\n", nativeColor(fd->color)) : std::string()) +
               "ISBUS 1 1\nNCHAN 2\n>\n";
      f.folderDepth = 1;
      f.color = fd->color;
      f.folderKey = grp;
      f.isFolder = true;
      out.push_back(f);
    }
    std::string c;
    c += "<TRACK\n";
    c += "NAME " + q(t.name) + "\n";
    uint32_t tcol = trackColor(t, opt);
    if (tcol) c += strprintf("PEAKCOL %u\n", nativeColor(tcol));
    c += "VOLPAN " + num(t.volume) + " " + num(t.pan) + " -1 -1 1\n";
    int nch = t.channels <= 2 ? 2 : (t.channels + 1) / 2 * 2;
    c += strprintf("NCHAN %d\n", nch);
    // a track put on an existing REAPER track keeps that track's envelopes, unless they are overwritten
    bool matched = !t.target.empty();
    bool envToTrack = !matched || opt.overwriteEnvelopes;
    std::vector<GainPoint> vol = trackVolumeEnvelope(t, opt);
    std::string volChunk, panChunk;
    if (!vol.empty()) {
      volChunk = "<VOLENV2\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
      for (auto &pt : vol) volChunk += "PT " + num(pt.time + opt.tcOffset) + " " + num(pt.gain) + " 0\n";
      volChunk += ">\n";
    }
    if (!t.panEnv.empty()) {
      // REAPER pan envelopes run the other way round: +1 = left, -1 = right
      panChunk = "<PANENV2\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
      for (auto &pt : t.panEnv) panChunk += "PT " + num(pt.time + opt.tcOffset) + " " + num(-pt.gain) + " 0\n";
      panChunk += ">\n";
    }
    c += volChunk + panChunk;
    std::vector<std::string> items;
    for (auto &k : t.clips) {
      if (k.excluded) continue;
      size_t itemStart = c.size();
      c += "<ITEM\n";
      c += "POSITION " + num(k.pos + opt.tcOffset) + "\n";
      c += "LENGTH " + num(k.len) + "\n";
      c += "LOOP 0\n";
      double fi = opt.fades ? k.fadeIn.len : 0.0, fo = opt.fades ? k.fadeOut.len : 0.0;
      c += strprintf("FADEIN %d %s 0\n", opt.fades ? k.fadeIn.shape : 0, num(fi).c_str());
      c += strprintf("FADEOUT %d %s 0\n", opt.fades ? k.fadeOut.shape : 0, num(fo).c_str());
      if (k.mute) c += "MUTE 1 0\n";
      c += "NAME " + q(k.name) + "\n";
      // clip gain on the item, or folded into the track envelope (not when that envelope is not written)
      bool onItem = opt.clipGain && (!opt.clipGainToTrack || !envToTrack);
      c += "VOLPAN " + num(onItem ? k.gain : 1.0) + " 0 1 -1\n";
      c += "SOFFS " + num(k.srcOffset) + "\n";
      if (k.chanMode) c += strprintf("CHANMODE %d\n", k.chanMode);
      std::string f = k.outPath;
      if (!opt.relativeTo.empty()) {
        std::string base = opt.relativeTo;
        if (base.back() != '/' && base.back() != '\\') base += "/";
        if (f.compare(0, base.size(), base) == 0) f = f.substr(base.size());
      }
      std::string type = lower(fileExt(f)) == "mxf" ? "VIDEO" : "WAVE";
      c += "<SOURCE " + type + "\n";
      c += "FILE " + q(f) + "\n";
      c += ">\n";
      if (onItem && !k.gainEnv.empty()) {
        // clip gain automation -> take volume envelope (times from item start, linear amplitude)
        c += "<VOLENV\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
        for (auto &pt : k.gainEnv) c += "PT " + num(pt.time) + " " + num(pt.gain) + " 0\n";
        c += ">\n";
      }
      c += ">\n";
      items.push_back(c.substr(itemStart));
    }
    c += ">\n";
    if (lastInGroup) {
      // ISBUS 2 -1 closes the folder
      size_t pos = c.find("\nNCHAN");
      c.insert(pos, "\nISBUS 2 -1");
    }
    TrackChunk tc;
    tc.text = c;
    tc.folderDepth = lastInGroup ? -1 : 0;
    tc.folderKey = grp;
    tc.color = tcol;
    tc.targetGuid = t.target;
    tc.items = std::move(items);
    if (matched && opt.overwriteEnvelopes) {
      for (auto &pt : vol) tc.volEnv.push_back({pt.time + opt.tcOffset, pt.gain});
      for (auto &pt : t.panEnv) tc.panEnv.push_back({pt.time + opt.tcOffset, -pt.gain});
      tc.volEnvChunk = volChunk;
      tc.panEnvChunk = panChunk;
      bool first = true;
      for (auto &k : t.clips) {
        if (k.excluded) continue;
        if (first || k.pos < tc.rangeStart) tc.rangeStart = k.pos;
        if (first || k.pos + k.len > tc.rangeEnd) tc.rangeEnd = k.pos + k.len;
        first = false;
      }
      for (auto &pt : vol) tc.rangeStart = std::min(tc.rangeStart, pt.time), tc.rangeEnd = std::max(tc.rangeEnd, pt.time);
      for (auto &pt : t.panEnv) tc.rangeStart = std::min(tc.rangeStart, pt.time), tc.rangeEnd = std::max(tc.rangeEnd, pt.time);
      tc.rangeStart += opt.tcOffset;
      tc.rangeEnd += opt.tcOffset;
    }
    out.push_back(tc);
  }
  return out;
}

std::string Importer::projectRpp(const ImportOptions &opt) const {
  std::string r = "<REAPER_PROJECT 0.1 \"7.0/OLAAF\" 0\n";
  r += strprintf("SAMPLERATE %d 0 0\n", s_.sampleRate);
  if (opt.markers) {
    int n = 1;
    for (auto &m : s_.markers)
      r += strprintf("MARKER %d %s %s 0\n", n++, num(m.pos + opt.tcOffset).c_str(), q(m.name.empty() ? m.comment : m.name).c_str());
  }
  for (auto &c : trackChunks(opt)) r += c.text; // (a new project: there are no existing tracks to match)
  r += ">\n";
  return r;
}

} // namespace olaaf
