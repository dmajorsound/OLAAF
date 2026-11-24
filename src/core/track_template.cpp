// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "track_template.h"
#include "util.h"

#include <fstream>
#include <set>
#include <sstream>

namespace olaaf {

static std::string trimmed(const std::string &l) {
  size_t a = l.find_first_not_of(" \t\r"), b = l.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : l.substr(a, b - a + 1);
}

static std::string unquote(std::string v) {
  v = trimmed(v);
  if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'' || v[0] == '`') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
  return v;
}

std::vector<TemplateTrack> parseTrackTemplate(const std::string &text) {
  std::vector<TemplateTrack> out;
  std::istringstream in(text);
  std::string line;
  int level = 0;
  TemplateTrack cur;
  while (std::getline(in, line)) {
    std::string t = trimmed(line);
    if (t.empty()) continue;
    if (level == 0) {
      if (t.rfind("<TRACK", 0) == 0) {
        cur = TemplateTrack();
        cur.chunk = "<TRACK\n";
        level = 1;
      }
      continue;
    }
    // a new GUID for everything: REAPER makes them when they are missing
    std::string head = t.substr(0, t.find(' '));
    if (head == "TRACKID" || head == "GUID" || head == "IGUID" || head == "FXID") continue;
    if (level == 1 && head == "NAME") cur.name = unquote(t.substr(4));
    if (level == 1 && head == "ISBUS") {
      int a = 0, b = 0;
      if (sscanf(t.c_str() + 5, "%d %d", &a, &b) == 2) cur.depth = a == 1 ? 1 : (a == 2 ? b : 0);
    }
    if (t[0] == '<') level++;
    else if (t == ">") level--;
    cur.chunk += t + "\n";
    if (level == 0) out.push_back(cur);
  }
  return out;
}

bool readTrackTemplate(const std::string &path, std::vector<TemplateTrack> *out, std::string *error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "The track template " + baseName(path) + " can't be opened.";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  *out = parseTrackTemplate(ss.str());
  if (out->empty()) {
    if (error) *error = baseName(path) + " holds no tracks.";
    return false;
  }
  return true;
}

std::map<std::string, int> matchTemplate(const std::vector<TemplateTrack> &tpl, const std::vector<Folder> &folders) {
  std::map<std::string, int> out;
  std::set<int> used;
  // the same name first ("FX" -> "FX"), then the same kind ("Dialog" -> DIA), folder tracks first
  for (auto &f : folders)
    for (size_t i = 0; i < tpl.size(); i++)
      if (!used.count((int)i) && lower(trimmed(tpl[i].name)) == lower(trimmed(f.name))) {
        out[f.key] = (int)i;
        used.insert((int)i);
        break;
      }
  for (int pass = 0; pass < 2; pass++)
    for (auto &f : folders) {
      if (out.count(f.key)) continue;
      std::string kind = trackGroup(f.name);
      if (kind == "OTHER") kind = f.key;
      for (size_t i = 0; i < tpl.size(); i++) {
        if (used.count((int)i) || (pass == 0 && tpl[i].depth != 1)) continue;
        if (tpl[i].name.empty() || trackGroup(tpl[i].name) != kind) continue;
        out[f.key] = (int)i;
        used.insert((int)i);
        break;
      }
    }
  return out;
}

std::vector<TrackChunk> applyTrackTemplate(const std::vector<TrackChunk> &chunks, const std::vector<TemplateTrack> &tpl,
                                           const std::vector<Folder> &folders, int base) {
  std::map<std::string, int> match = matchTemplate(tpl, folders);
  std::map<int, std::vector<const TrackChunk *>> into; // template index -> AAF tracks going in
  for (auto &c : chunks)
    if (c.targetGuid.empty() && !c.isFolder && match.count(c.folderKey)) into[match[c.folderKey]].push_back(&c);

  std::vector<TrackChunk> out, rest;
  for (auto &c : chunks) {
    if (!c.targetGuid.empty()) { out.push_back(c); continue; } // on existing tracks: no new index
    if (match.count(c.folderKey)) continue;                    // in the template (or its folder track, not needed)
    if (c.text.find("NAME \"VIDEO\"") != std::string::npos && c.folderKey.empty() && rest.empty()) out.push_back(c);
    else rest.push_back(c);
  }
  // where each template track ends up (for the sends between them)
  int at = 0;
  for (auto &c : out)
    if (c.targetGuid.empty()) at++;
  std::vector<int> pos(tpl.size());
  {
    int p = at;
    for (size_t i = 0; i < tpl.size(); i++) {
      pos[i] = p;
      p += 1 + (into.count((int)i) ? (int)into[(int)i].size() : 0);
    }
  }
  int sum = 0;
  size_t firstTpl = out.size();
  for (size_t i = 0; i < tpl.size(); i++) {
    TrackChunk t;
    std::string text;
    std::istringstream in(tpl[i].chunk);
    std::string line;
    while (std::getline(in, line)) { // AUXRECV <source track> ...: renumber the source
      if (line.rfind("AUXRECV ", 0) == 0) {
        int src = atoi(line.c_str() + 8);
        size_t sp = line.find(' ', 8);
        if (src >= 0 && src < (int)tpl.size() && sp != std::string::npos)
          line = "AUXRECV " + std::to_string(base + pos[(size_t)src]) + line.substr(sp);
      }
      text += line + "\n";
    }
    t.text = text;
    t.folderDepth = tpl[i].depth;
    auto kids = into.find((int)i);
    if (kids == into.end() || kids->second.empty()) {
      sum += t.folderDepth;
      out.push_back(t);
      continue;
    }
    int closes = tpl[i].depth == 1 ? 0 : tpl[i].depth - 1; // a plain track becomes a folder
    t.folderDepth = 1;
    out.push_back(t);
    sum += 1;
    for (size_t k = 0; k < kids->second.size(); k++) {
      TrackChunk c = *kids->second[k];
      c.folderDepth = k + 1 == kids->second.size() ? closes : 0;
      sum += c.folderDepth;
      out.push_back(c);
    }
  }
  // a template that leaves folders open: close them before the other AAF tracks
  if (sum > 0 && out.size() > firstTpl) out.back().folderDepth -= sum;
  for (auto &c : rest) out.push_back(c);
  return out;
}

} // namespace olaaf
