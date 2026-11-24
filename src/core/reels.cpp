// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "reels.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace olaaf {

std::string reelLabel(const Session &s, int index) {
  std::string n = stemName(s.aafPath);
  return n.empty() ? strprintf("Reel %d", index + 1) : n;
}

static bool isSep(char c) { return c == '_' || c == ' ' || c == '-' || c == '.'; }

std::vector<std::string> reelShortLabels(const std::vector<std::string> &names) {
  if (names.size() < 2) return names;
  size_t pre = names[0].size(), suf = names[0].size();
  for (auto &n : names) {
    size_t k = 0;
    while (k < pre && k < n.size() && n[k] == names[0][k]) k++;
    pre = k;
    k = 0;
    while (k < suf && k < n.size() && n[n.size() - 1 - k] == names[0][names[0].size() - 1 - k]) k++;
    suf = k;
  }
  // cut at word boundaries; the word just before the difference stays ("Rolle_1", not "1")
  size_t cut = 0, prev = 0;
  for (size_t i = 0; i < pre; i++)
    if (isSep(names[0][i])) {
      prev = cut;
      cut = i + 1;
    }
  cut = prev;
  size_t sufCut = 0;
  for (size_t i = 0; i < suf; i++)
    if (isSep(names[0][names[0].size() - 1 - i])) sufCut = i + 1;
  std::vector<std::string> out;
  for (auto &n : names) {
    size_t b = std::min(cut, n.size()), e = n.size() >= sufCut ? n.size() - sufCut : n.size();
    std::string v = e > b ? n.substr(b, e - b) : n;
    out.push_back(v.empty() ? n : v);
  }
  return out;
}

bool reelsOverlap(double a0, double a1, double b0, double b1) { return a0 < b1 - 1e-6 && b0 < a1 - 1e-6; }

static std::string key(const std::string &name) {
  std::string k = lower(name);
  size_t a = k.find_first_not_of(" \t"), b = k.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : k.substr(a, b - a + 1);
}

Session combineReels(const std::vector<Session> &reels, const std::vector<double> &shift) {
  if (reels.empty()) return Session();
  Session out = reels[0];
  auto sh = [&](size_t i) { return i < shift.size() ? shift[i] : 0.0; };
  auto shiftAll = [](Session &s, double d) {
    if (d == 0) return;
    for (auto &t : s.tracks) {
      for (auto &c : t.clips) c.pos += d;
      for (auto &p : t.volEnv) p.time += d;
      for (auto &p : t.panEnv) p.time += d;
    }
    for (auto &v : s.video) v.pos += d;
    for (auto &m : s.markers) m.pos += d;
    s.start += d;
    s.end += d;
  };
  shiftAll(out, sh(0));
  int maxId = 0, maxUid = 0;
  for (auto &t : out.tracks) {
    maxUid = std::max(maxUid, t.uid);
    for (auto &c : t.clips) maxId = std::max(maxId, c.id);
  }
  out.warnings.clear();
  for (auto &w : reels[0].warnings) out.warnings.push_back(reelLabel(reels[0], 0) + ": " + w);

  for (size_t r = 1; r < reels.size(); r++) {
    Session s = reels[r];
    shiftAll(s, sh(r));
    std::string label = reelLabel(s, (int)r);
    for (auto &w : s.warnings) out.warnings.push_back(label + ": " + w);
    int mediaBase = (int)out.media.size();
    out.media.insert(out.media.end(), s.media.begin(), s.media.end());
    for (auto &f : s.folders)
      if (!out.folder(f.key)) out.folders.push_back(f);
    std::map<int, int> uidMap; // this reel's track uid -> the combined one
    int idBase = maxId;
    for (auto &t : s.tracks) {
      if (!t.import) continue;
      for (auto &c : t.clips) {
        c.id += idBase;
        maxId = std::max(maxId, c.id);
        for (auto &src : c.sources)
          if (src.media >= 0) src.media += mediaBase;
      }
      // the same name (and the same "put on" target): one track
      Track *same = nullptr;
      for (auto &o : out.tracks)
        if (o.import && key(o.name) == key(t.name) && o.target == t.target) {
          same = &o;
          break;
        }
      if (!same) {
        Track n = t;
        n.uid = ++maxUid;
        uidMap[t.uid] = n.uid;
        out.tracks.push_back(n);
        continue;
      }
      uidMap[t.uid] = same->uid;
      // a different fader level: the reel's events keep their level through their clip gain
      if (std::fabs(t.volume - same->volume) > 1e-6 && same->volume > 0)
        for (auto &c : t.clips) c.gain *= t.volume / same->volume;
      if (std::fabs(t.pan - same->pan) > 1e-6 && t.panEnv.empty())
        out.warnings.push_back(label + ": \"" + t.name + "\" is panned differently; the first reel's pan is used.");
      if (same->volEnv.empty() != t.volEnv.empty() || same->panEnv.empty() != t.panEnv.empty())
        out.warnings.push_back(label + ": \"" + t.name + "\" has volume or pan automation in only some reels; check the level where the reels meet.");
      same->channels = std::max(same->channels, t.channels);
      same->clips.insert(same->clips.end(), t.clips.begin(), t.clips.end());
      same->volEnv.insert(same->volEnv.end(), t.volEnv.begin(), t.volEnv.end());
      same->panEnv.insert(same->panEnv.end(), t.panEnv.begin(), t.panEnv.end());
      auto byTime = [](const GainPoint &a, const GainPoint &b) { return a.time < b.time; };
      std::stable_sort(same->volEnv.begin(), same->volEnv.end(), byTime);
      std::stable_sort(same->panEnv.begin(), same->panEnv.end(), byTime);
      if (same->group.empty()) same->group = t.group;
    }
    for (auto &t : out.tracks)
      for (auto &c : t.clips)
        if (c.id > idBase) {
          auto m = uidMap.find(c.home);
          if (m != uidMap.end()) c.home = m->second;
        }
    for (auto &v : s.video) out.video.push_back(v);
    out.markers.insert(out.markers.end(), s.markers.begin(), s.markers.end());
    out.start = std::min(out.start, s.start);
    out.end = std::max(out.end, s.end);
  }
  for (auto &t : out.tracks)
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip &a, const Clip &b) { return a.pos < b.pos; });
  return out;
}

} // namespace olaaf
