// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "importer.h"
#include "audiofile.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <set>

namespace olaaf {

std::string defaultSubdir(const Session &s) {
  return safeFileName((s.name.empty() ? stemName(s.aafPath) : s.name) + "_Audio");
}

bool Importer::load(const std::string &aafPath, const ImportOptions &opt, std::string *error) {
  s_ = Session();
  aaf_.reset(new AafFile());
  return aaf_->load(aafPath, opt.read, &s_, error);
}

// ------------------------------------------------------------------ locate

void Importer::locateMedia(const ImportOptions &opt) {
  std::vector<std::pair<std::string, std::string>> index;
  std::vector<std::string> folders = opt.searchFolders;
  folders.push_back(dirName(s_.aafPath));
  for (auto &f : folders) indexFolder(f, 4, &index);
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

  for (auto &m : s_.media) {
    if (m.kind == MediaKind::Embedded) continue;
    if (fileExists(m.foundPath)) continue;
    m.foundPath.clear();
    std::string p = uriToPath(m.originalUri);
    if (fileExists(p)) {
      m.foundPath = p;
    } else {
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

std::vector<std::string> Importer::trackChunks(const ImportOptions &opt) const {
  std::vector<std::string> out;
  for (auto &t : s_.tracks) {
    if (!t.import) continue;
    std::string c;
    c += "<TRACK\n";
    c += "NAME " + q(t.name) + "\n";
    if (t.color) c += strprintf("PEAKCOL %u\n", nativeColor(t.color));
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
    out.push_back(c);
  }
  return out;
}

std::string Importer::projectRpp(const ImportOptions &opt) const {
  std::string r = "<REAPER_PROJECT 0.1 \"7.0/olAAF\" 0\n";
  r += strprintf("SAMPLERATE %d 0 0\n", s_.sampleRate);
  for (auto &c : trackChunks(opt)) r += c;
  r += ">\n";
  return r;
}

} // namespace olaaf
