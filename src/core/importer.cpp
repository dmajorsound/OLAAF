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

static std::vector<std::string> tokens(const std::string &name) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : lower(name)) {
    if (std::isalnum(c) || c >= 0x80) cur += (char)c;
    else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string trackGroup(const std::string &trackName) {
  static const struct { const char *group; std::vector<std::string> keys; } table[] = {
      {"MUSIC", {"mus", "music", "musik", "mx", "score", "song"}},
      {"ATMO", {"atmo", "atmos", "amb", "ambi", "ambience", "bg", "bgs", "roomtone", "rt"}},
      {"FOLEY", {"foley", "fol", "ger\xc3\xa4usche", "ger\xc3\xa4usch", "gerausche", "steps", "fs"}},
      {"FX", {"sfx", "fx", "effect", "effects", "hardfx", "design", "sd"}},
      {"DIALOG", {"dia", "dx", "dlg", "dial", "dialog", "dialogue", "boom", "lav", "anstecker", "fr", "vo", "adr",
                  "voice", "ot", "guide", "sprache", "mic"}},
  };
  auto tk = tokens(trackName);
  for (auto &row : table)
    for (auto &t : tk)
      for (auto &k : row.keys)
        if (t == k || (k.size() >= 4 && t.compare(0, k.size(), k) == 0)) return row.group;
  return "OTHER";
}

uint32_t groupColor(const std::string &g) {
  if (g == "DIALOG") return 0x5580d4;
  if (g == "FOLEY") return 0x7A5AC8;
  if (g == "FX") return 0xD9822B;
  if (g == "ATMO") return 0x1F9A8A;
  if (g == "MUSIC") return 0xC8423B;
  return 0x6B7480;
}

Folder *Session::folder(const std::string &key) {
  for (auto &f : folders)
    if (f.key == key) return &f;
  return nullptr;
}

static const char *kDefaultFolders[] = {"DIALOG", "FOLEY", "FX", "ATMO", "MUSIC", "OTHER"};

void Importer::assignDefaultFolders() {
  std::set<std::string> present;
  for (auto &t : s_.tracks) present.insert(trackGroup(t.name));
  s_.folders.clear();
  if (present.size() < 2) { // one folder for everything does not help
    for (auto &t : s_.tracks) t.group.clear();
    return;
  }
  for (const char *g : kDefaultFolders)
    if (present.count(g)) s_.folders.push_back({g, g, groupColor(g)});
  for (auto &t : s_.tracks) t.group = trackGroup(t.name);
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
  for (auto &t : s_.tracks)
    for (auto &c : t.clips) c.id = id++;
  assignDefaultFolders();
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
    if (home && d == home) return true;
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

int Importer::addTrack(int like) {
  Track t;
  int n = 1;
  for (auto &x : s_.tracks)
    if (x.created) n++;
  t.name = strprintf("New track %d", n);
  t.created = true;
  if (like >= 0 && like < (int)s_.tracks.size()) {
    t.channels = s_.tracks[(size_t)like].channels;
    t.group = s_.tracks[(size_t)like].group;
  }
  s_.tracks.push_back(t);
  return (int)s_.tracks.size() - 1;
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
      L.pan = 0.0; // the hard L / hard R pan of the halves is now the stereo file itself
      if (!L.panEnv.empty() || !R.panEnv.empty())
        s_.warnings.push_back("\"" + bl + "\": pan automation of the L/R tracks is not carried over to the stereo track.");
      L.panEnv.clear();
      L.name = bl;
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
      const Media &m0 = s_.media[(size_t)c.sources[0].media];
      if (c.sources.size() == 1) {
        c.outPath = m0.outPath;
        int ch = c.sources[0].channel;
        if (ch > 0 && m0.channels == 2) c.chanMode = ch == 1 ? 3 : 4;
        else if (ch > 0 && m0.channels > 2) c.chanMode = 2 + ch; // REAPER: 3+ = mono channel n-2
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
    r.logPath = joinPath(target, "olAAF import log.txt");
    if (FILE *lf = openUtf8(r.logPath, "wb")) {
      fprintf(lf, "olAAF import log\nAAF: %s\n%s %s %s\n%d Hz, %.3g fps, start %s\ndestination: %s\n\n", s_.aafPath.c_str(),
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
      v.text += "<ITEM\nPOSITION " + num(vc.pos + opt.tcOffset) + "\nLENGTH " + num(vc.len) + "\nLOOP 0\n";
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
        if (t.import && t.group == f.key && !emptyCreated(t)) order.push_back({f.key, &t});
    for (auto &t : s_.tracks)
      if (t.import && !emptyCreated(t) && (t.group.empty() || !const_cast<Session &>(s_).folder(t.group)))
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
    if (!t.volEnv.empty()) {
      c += "<VOLENV2\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
      for (auto &pt : t.volEnv) c += "PT " + num(pt.time + opt.tcOffset) + " " + num(pt.gain) + " 0\n";
      c += ">\n";
    }
    if (!t.panEnv.empty()) {
      // REAPER pan envelopes run the other way round: +1 = left, -1 = right
      c += "<PANENV2\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
      for (auto &pt : t.panEnv) c += "PT " + num(pt.time + opt.tcOffset) + " " + num(-pt.gain) + " 0\n";
      c += ">\n";
    }
    for (auto &k : t.clips) {
      if (k.excluded) continue;
      c += "<ITEM\n";
      c += "POSITION " + num(k.pos + opt.tcOffset) + "\n";
      c += "LENGTH " + num(k.len) + "\n";
      c += "LOOP 0\n";
      double fi = opt.fades ? k.fadeIn.len : 0.0, fo = opt.fades ? k.fadeOut.len : 0.0;
      c += strprintf("FADEIN %d %s 0\n", opt.fades ? k.fadeIn.shape : 0, num(fi).c_str());
      c += strprintf("FADEOUT %d %s 0\n", opt.fades ? k.fadeOut.shape : 0, num(fo).c_str());
      if (k.mute) c += "MUTE 1 0\n";
      c += "NAME " + q(k.name) + "\n";
      c += "VOLPAN " + num(opt.clipGain ? k.gain : 1.0) + " 0 1 -1\n";
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
      if (opt.clipGain && !k.gainEnv.empty()) {
        // clip gain automation -> take volume envelope (times from item start, linear amplitude)
        c += "<VOLENV\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n";
        for (auto &pt : k.gainEnv) c += "PT " + num(pt.time) + " " + num(pt.gain) + " 0\n";
        c += ">\n";
      }
      c += ">\n";
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
    tc.color = tcol;
    out.push_back(tc);
  }
  return out;
}

std::string Importer::projectRpp(const ImportOptions &opt) const {
  std::string r = "<REAPER_PROJECT 0.1 \"7.0/olAAF\" 0\n";
  r += strprintf("SAMPLERATE %d 0 0\n", s_.sampleRate);
  if (opt.markers) {
    int n = 1;
    for (auto &m : s_.markers)
      r += strprintf("MARKER %d %s %s 0\n", n++, num(m.pos + opt.tcOffset).c_str(), q(m.name.empty() ? m.comment : m.name).c_str());
  }
  for (auto &c : trackChunks(opt)) r += c.text;
  r += ">\n";
  return r;
}

} // namespace olaaf
