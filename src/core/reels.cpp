// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "reels.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cctype>
#include <cstring>
#include <set>

namespace olaaf {

std::string reelLabel(const Session &s, int index) {
  std::string n = stemName(s.aafPath);
  return n.empty() ? strprintf("Reel %d", index + 1) : n;
}

static bool isSep(char c) { return c == '_' || c == ' ' || c == '-' || c == '.'; }

// "Film_Reel1_v3" -> "Reel 1", "Rolle_02" -> "Rolle 2" ("" when the name has no reel number)
static std::string reelWord(const std::string &name) {
  static const char *words[] = {"reel", "rolle", "akt", "act", "part", "teil"};
  std::string l = lower(name);
  for (const char *w : words) {
    size_t n = strlen(w);
    for (size_t at = l.find(w); at != std::string::npos; at = l.find(w, at + 1)) {
      if (at > 0 && isalpha((unsigned char)l[at - 1])) continue; // inside another word
      size_t p = at + n;
      while (p < l.size() && (l[p] == ' ' || l[p] == '_' || l[p] == '-' || l[p] == '.')) p++;
      size_t d = p;
      while (d < l.size() && isdigit((unsigned char)l[d])) d++;
      if (d == p || (d < l.size() && isalpha((unsigned char)l[d]))) continue;
      std::string word = name.substr(at, n);
      word[0] = (char)toupper((unsigned char)word[0]);
      return word + " " + std::to_string(atoi(l.substr(p, d - p).c_str()));
    }
  }
  return "";
}

std::vector<std::string> reelNumberLabels(const std::vector<std::string> &names) {
  std::vector<std::string> r;
  std::set<std::string> seen;
  for (auto &n : names) r.push_back(reelWord(n));
  for (auto &x : r)
    if (x.empty() || !seen.insert(x).second) return {};
  return r;
}

std::vector<std::string> reelShortLabels(const std::vector<std::string> &names) {
  if (names.size() < 2) return names;
  // the names number their reels ("..._Reel1_...", "Rolle 2"): "Reel 1", "Rolle 2", if that tells them apart
  std::vector<std::string> r = reelNumberLabels(names);
  if (!r.empty()) return r;
  // otherwise the part that differs ("ri8_ftr1_finalblock", "ri8_ftr2_finalblock" -> "ftr1", "ftr2");
  // names from different projects have nothing in common and stay whole
  size_t pre = names[0].size(), suf = names[0].size();
  for (auto &n : names) {
    size_t k = 0;
    while (k < pre && k < n.size() && n[k] == names[0][k]) k++;
    pre = k;
    k = 0;
    while (k < suf && k < n.size() && n[n.size() - 1 - k] == names[0][names[0].size() - 1 - k]) k++;
    suf = k;
  }
  size_t cut = 0, prev = 0;
  for (size_t i = 0; i < pre; i++)
    if (isSep(names[0][i])) {
      prev = cut;
      cut = i + 1;
    }
  size_t sufCut = 0;
  for (size_t i = 0; i < suf; i++)
    if (isSep(names[0][names[0].size() - 1 - i])) sufCut = i + 1;
  std::vector<std::string> out;
  bool tooShort = false;
  for (auto &n : names) {
    size_t b = std::min(cut, n.size()), e = n.size() >= sufCut ? n.size() - sufCut : n.size();
    std::string v = e > b ? n.substr(b, e - b) : n;
    tooShort = tooShort || v.size() <= 2;
    out.push_back(v.empty() ? n : v);
  }
  if (tooShort) { // "1", "2": the word before it stays ("Take_1")
    out.clear();
    for (auto &n : names) {
      size_t b = std::min(prev, n.size()), e = n.size() >= sufCut ? n.size() - sufCut : n.size();
      out.push_back(e > b ? n.substr(b, e - b) : n);
    }
  }
  return out;
}

bool reelsOverlap(double a0, double a1, double b0, double b1) { return a0 < b1 - 1e-6 && b0 < a1 - 1e-6; }

static std::string key(const std::string &name) {
  std::string k = lower(name);
  size_t a = k.find_first_not_of(" \t"), b = k.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : k.substr(a, b - a + 1);
}

static std::set<std::string> trackKeys(const Session &s) {
  std::set<std::string> k;
  for (auto &t : s.tracks)
    if (!key(t.name).empty()) k.insert(key(t.name));
  return k;
}

bool reelsBelongTogether(const std::vector<Session> &reels, std::string *why) {
  if (reels.size() < 2) return false;
  std::vector<std::string> names;
  for (size_t i = 0; i < reels.size(); i++) names.push_back(reelLabel(reels[i], (int)i));
  if (!reelNumberLabels(names).empty()) {
    if (why) *why = "their names number the reels";
    return true;
  }
  std::set<std::string> a = trackKeys(reels[0]);
  for (size_t r = 1; r < reels.size(); r++) {
    std::set<std::string> b = trackKeys(reels[r]);
    size_t common = 0;
    for (auto &k : b) common += a.count(k);
    size_t smaller = std::min(a.size(), b.size());
    if (smaller == 0 || common * 10 < smaller * 6) return false; // less than 60 % of the names in common
  }
  if (why) *why = "they have the same track layout";
  return true;
}

// the value of an envelope at a time (straight lines between the points)
static double envValue(const std::vector<GainPoint> &env, double t, double def) {
  if (env.empty()) return def;
  if (t <= env.front().time) return env.front().gain;
  if (t >= env.back().time) return env.back().gain;
  for (size_t i = 1; i < env.size(); i++)
    if (env[i].time >= t) {
      double a = env[i - 1].time, b = env[i].time;
      double f = b > a ? (t - a) / (b - a) : 1.0;
      return env[i - 1].gain + (env[i].gain - env[i - 1].gain) * f;
    }
  return def;
}

// One reel's part of a shared track: its automation only counts between its start and end.
struct EnvPart {
  double start = 0, end = 0;
  std::vector<GainPoint> vol, pan;
  double volume = 1, pan0 = 0;
};

static std::vector<GainPoint> joinParts(const std::vector<EnvPart> &parts, bool pan) {
  std::vector<GainPoint> out;
  for (auto &p : parts) {
    const std::vector<GainPoint> &env = pan ? p.pan : p.vol;
    double def = pan ? p.pan0 : p.volume;
    out.push_back({p.start, envValue(env, p.start, def)});
    for (auto &pt : env)
      if (pt.time > p.start && pt.time < p.end) out.push_back(pt);
    out.push_back({p.end, envValue(env, p.end, def)});
  }
  std::stable_sort(out.begin(), out.end(), [](const GainPoint &a, const GainPoint &b) { return a.time < b.time; });
  return out;
}

Session combineReels(const std::vector<Session> &reels, const std::vector<double> &shift, bool shareTracks) {
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
  // every reel's automation per combined track (uid), to keep each inside its own time
  std::map<int, std::vector<EnvPart>> parts;
  auto partOf = [](const Track &t, const Session &s) {
    EnvPart p;
    p.start = s.start;
    p.end = s.end;
    p.vol = t.volEnv;
    p.pan = t.panEnv;
    p.volume = t.volume;
    p.pan0 = t.pan;
    return p;
  };
  for (auto &t : out.tracks) parts[t.uid].push_back(partOf(t, out));
  // short reel names for tracks of their own: "Reel 2", or the part of the AAF name that differs
  std::vector<std::string> allNames;
  for (size_t i = 0; i < reels.size(); i++) allNames.push_back(reelLabel(reels[i], (int)i));
  std::vector<std::string> shortNames = reelShortLabels(allNames);

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
      // the same name (and the same "put on" target): one track, unless every AAF keeps its own tracks
      Track *same = nullptr;
      if (shareTracks)
        for (auto &o : out.tracks)
          if (o.import && key(o.name) == key(t.name) && o.target == t.target) {
            same = &o;
            break;
          }
      if (!same) {
        Track n = t;
        n.uid = ++maxUid;
        if (!shareTracks) { // its own track: the reel in the name, so the tracks can be told apart in REAPER
          if (n.aafName.empty()) n.aafName = n.name;
          n.name += " (" + (r < shortNames.size() ? shortNames[r] : label) + ")";
        }
        uidMap[t.uid] = n.uid;
        out.tracks.push_back(n);
        parts[n.uid].push_back(partOf(t, s));
        continue;
      }
      uidMap[t.uid] = same->uid;
      // a different fader level: the reel's events keep their level through their clip gain
      // (only without automation of its own: then the automation sets the level)
      if (t.volEnv.empty() && std::fabs(t.volume - same->volume) > 1e-6 && same->volume > 0)
        for (auto &c : t.clips) c.gain *= t.volume / same->volume;
      EnvPart p = partOf(t, s);
      if (p.vol.empty()) p.volume = same->volume; // its level is in the clip gain now
      parts[same->uid].push_back(p);
      same->channels = std::max(same->channels, t.channels);
      same->clips.insert(same->clips.end(), t.clips.begin(), t.clips.end());
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
  // shared tracks: each reel's volume and pan automation only within its own time, the others' left alone
  // (a reel without automation keeps its level and pan there)
  for (auto &t : out.tracks) {
    auto it = parts.find(t.uid);
    if (it == parts.end() || it->second.size() < 2) continue;
    bool anyVol = false, anyPan = false;
    for (auto &p : it->second) {
      anyVol = anyVol || !p.vol.empty();
      anyPan = anyPan || !p.pan.empty() || std::fabs(p.pan0 - it->second[0].pan0) > 1e-6;
    }
    std::vector<EnvPart> ps = it->second;
    std::stable_sort(ps.begin(), ps.end(), [](const EnvPart &a, const EnvPart &b) { return a.start < b.start; });
    bool overlap = false;
    for (size_t k = 1; k < ps.size(); k++) overlap = overlap || reelsOverlap(ps[k - 1].start, ps[k - 1].end, ps[k].start, ps[k].end);
    if (overlap && (anyVol || anyPan))
      out.warnings.push_back("\"" + t.name + "\": reels overlap in time, so their automation on this shared track overlaps too.");
    if (anyVol) t.volEnv = joinParts(ps, false);
    if (anyPan) t.panEnv = joinParts(ps, true);
  }
  for (auto &t : out.tracks)
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip &a, const Clip &b) { return a.pos < b.pos; });
  return out;
}

} // namespace olaaf
