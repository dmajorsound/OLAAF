// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// A REAPER track template (.RTrackTemplate) added on import: its tracks (stems, buses, routing)
// come first, and each AAF folder goes into the template track of the same kind ("Dialog" takes DX).

#pragma once
#include "importer.h"

#include <map>
#include <string>
#include <vector>

namespace olaaf {

struct TemplateTrack {
  std::string name;
  int depth = 0;     // REAPER I_FOLDERDEPTH from ISBUS
  std::string chunk; // "<TRACK ... >" without TRACKID / GUID lines
};

std::vector<TemplateTrack> parseTrackTemplate(const std::string &text);
bool readTrackTemplate(const std::string &path, std::vector<TemplateTrack> *out, std::string *error);

// folder key -> index of the template track its tracks go into (folders without one are left out)
std::map<std::string, int> matchTemplate(const std::vector<TemplateTrack> &tpl, const std::vector<Folder> &folders);

// The tracks to create, with the template in: the VIDEO track, the template tracks with the matching
// AAF tracks inside them, then the AAF tracks no template track took. `base` is the index of the first
// new REAPER track (for the sends between template tracks).
std::vector<TrackChunk> applyTrackTemplate(const std::vector<TrackChunk> &chunks, const std::vector<TemplateTrack> &tpl,
                                           const std::vector<Folder> &folders, int base);

} // namespace olaaf
