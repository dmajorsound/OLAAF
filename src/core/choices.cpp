// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "choices.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace olaaf {

static uint64_t fnv(const std::string &s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string choicesFileName(const std::string &aafPath) {
  std::string p = aafPath;
  for (auto &c : p)
    if (c == '\\') c = '/';
  return strprintf("%016llx.txt", (unsigned long long)fnv(lower(p)));
}

// the AAF's events, so choices are only put back onto the same edit
static std::string fingerprint(const Session &s) {
  std::vector<std::string> rows;
  for (auto &t : s.tracks)
    for (auto &c : t.clips) rows.push_back(strprintf("%d:%lld:%lld", c.id, llround(c.pos * 1000), llround(c.len * 1000)));
  std::sort(rows.begin(), rows.end());
  uint64_t h = 1469598103934665603ull;
  for (auto &r : rows) h = fnv(r + ";", h);
  return strprintf("%zu-%016llx", rows.size(), (unsigned long long)h);
}

// tabs and line breaks can't be in a field
static std::string field(std::string v) {
  for (auto &c : v)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return v;
}

static std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> out;
  size_t a = 0;
  while (true) {
    size_t b = line.find('\t', a);
    out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

std::string choicesToText(const Session &s, const ImportOptions &opt) {
  std::string out = "OLAAF choices 1\n";
  out += "aaf\t" + field(s.aafPath) + "\n";
  out += "events\t" + fingerprint(s) + "\n";
  out += strprintf("options\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", opt.mergeStereo, opt.fades, opt.markers, opt.folders,
                   opt.clipGain, opt.clipGainToTrack, opt.copyToSubdir, opt.video, opt.overwriteEnvelopes);
  out += "subdir\t" + field(opt.subdir) + "\n";
  if (!s.video.empty() && !s.video[0].foundPath.empty())
    out += strprintf("video\t%d\t%.6f\t", s.video[0].manual, s.video[0].pos) + field(s.video[0].foundPath) + "\n";
  for (auto &f : s.folders) out += "folder\t" + field(f.key) + strprintf("\t%06x\t", f.color) + field(f.name) + "\n";
  for (auto &t : s.tracks) {
    out += strprintf("track\t%d\t%d\t%d\t%d\t%06x\t", t.uid, t.created, t.import, t.channels, t.color) + field(t.group) + "\t" +
           field(t.target) + "\t" + field(t.targetName) + "\t" + field(t.name) + "\n";
    std::string ev = "events";
    for (auto &c : t.clips) ev += strprintf("\t%d:%d", c.id, (c.excluded ? 1 : 0) | (c.swapLR ? 2 : 0) | (c.mute ? 4 : 0));
    out += ev + "\n";
  }
  return out;
}

bool applyChoices(const std::string &text, Importer &imp, ImportOptions &opt, std::string *why) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != "OLAAF choices 1") return false;
  std::vector<std::vector<std::string>> rows;
  while (std::getline(in, line))
    if (!line.empty()) rows.push_back(splitTabs(line));
  auto find = [&](const char *k) -> const std::vector<std::string> * {
    for (auto &r : rows)
      if (r[0] == k) return &r;
    return nullptr;
  };
  const auto *ev = find("events");
  const auto *op = find("options");
  if (!ev || ev->size() < 2 || !op || op->size() < 10) return false;

  ImportOptions o = opt;
  o.mergeStereo = (*op)[1] == "1";
  // the .L / .R setting first: it decides which tracks there are
  bool mergeWas = opt.mergeStereo;
  if (o.mergeStereo != mergeWas) imp.setMergeStereo(o.mergeStereo);
  Session &s = imp.session();
  if (fingerprint(s) != (*ev)[1]) {
    if (o.mergeStereo != mergeWas) imp.setMergeStereo(mergeWas);
    if (why) *why = "This AAF changed since the last import: it starts fresh.";
    return false;
  }
  o.fades = (*op)[2] == "1";
  o.markers = (*op)[3] == "1";
  o.folders = (*op)[4] == "1";
  o.clipGain = (*op)[5] == "1";
  o.clipGainToTrack = (*op)[6] == "1";
  o.copyToSubdir = (*op)[7] == "1";
  o.video = (*op)[8] == "1";
  o.overwriteEnvelopes = (*op)[9] == "1";
  if (const auto *sd = find("subdir")) o.subdir = sd->size() > 1 ? (*sd)[1] : "";

  std::map<int, const Track *> byUid;
  std::map<int, const Clip *> byId;
  for (auto &t : s.tracks) {
    byUid[t.uid] = &t;
    for (auto &c : t.clips) byId[c.id] = &c;
  }
  std::vector<Track> tracks;
  std::vector<Folder> folders;
  std::set<int> placed;
  bool bad = false;
  for (size_t i = 0; i < rows.size() && !bad; i++) {
    const auto &r = rows[i];
    if (r[0] == "folder" && r.size() >= 4) {
      Folder f{r[1], r[3], (uint32_t)strtoul(r[2].c_str(), nullptr, 16)};
      std::string key = f.key;
      uint32_t c = f.color;
      if (migrateFolder(key, c)) { // a folder saved by an older version
        if (f.name == f.key) f.name = key;
        f.key = key;
        f.color = c;
      }
      folders.push_back(f);
    }
    if (r[0] != "track") continue;
    if (r.size() < 10) { bad = true; break; }
    int uid = atoi(r[1].c_str());
    bool created = r[2] == "1";
    Track t;
    if (!created) {
      auto it = byUid.find(uid);
      if (it == byUid.end()) { bad = true; break; }
      t = *it->second;
      t.clips.clear();
    } else {
      t.created = true;
      t.uid = uid;
      t.channels = atoi(r[4].c_str());
    }
    t.import = r[3] == "1";
    t.color = (uint32_t)strtoul(r[5].c_str(), nullptr, 16);
    t.group = r[6];
    if (!t.group.empty()) { // older versions: DIA/MUS/ATMO, and the folder's old color copied onto the track
      uint32_t c = t.color;
      std::string g = t.group;
      migrateFolder(g, c);
      t.group = g;
      if (c != t.color) t.color = 0; // the track takes its folder's color again
    }
    t.target = r[7];
    t.targetName = r[8];
    if (!r[9].empty()) t.name = r[9];
    if (i + 1 < rows.size() && rows[i + 1][0] == "events")
      for (size_t k = 1; k < rows[i + 1].size(); k++) {
        int id = 0, fl = 0;
        if (sscanf(rows[i + 1][k].c_str(), "%d:%d", &id, &fl) != 2) continue;
        auto c = byId.find(id);
        if (c == byId.end() || placed.count(id)) { bad = true; break; }
        Clip cl = *c->second;
        cl.excluded = fl & 1;
        cl.swapLR = (fl & 2) != 0;
        cl.mute = (fl & 4) != 0;
        t.clips.push_back(cl);
        placed.insert(id);
      }
    tracks.push_back(t);
  }
  if (bad || placed.size() != byId.size()) {
    if (o.mergeStereo != mergeWas) imp.setMergeStereo(mergeWas);
    if (why) *why = "This AAF changed since the last import: it starts fresh.";
    return false;
  }
  for (auto &t : tracks) // a folder that is gone: the track goes outside any folder
    if (!t.group.empty() && std::none_of(folders.begin(), folders.end(), [&](const Folder &f) { return f.key == t.group; }))
      t.group.clear();
  s.tracks = std::move(tracks);
  s.folders = std::move(folders);
  if (const auto *v = find("video"))
    if (v->size() >= 4 && fileExists((*v)[3])) {
      if ((*v)[1] == "1") {
        VideoClip vc;
        vc.manual = true;
        vc.foundPath = (*v)[3];
        vc.name = baseName(vc.foundPath);
        vc.pos = atof((*v)[2].c_str());
        s.video.clear();
        s.video.push_back(vc);
      } else if (!s.video.empty() && !fileExists(s.video[0].foundPath))
        s.video[0].foundPath = (*v)[3];
    }
  opt = o;
  if (why) *why = "Your choices from the last import of this AAF are back.";
  return true;
}

} // namespace olaaf
