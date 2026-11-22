// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The import pipeline shared by the REAPER extension and the command line tool:
//   load AAF -> merge .L/.R -> prepare media (locate, copy, convert) -> REAPER track chunks

#pragma once
#include "aaf_reader.h"
#include "model.h"

#include <functional>
#include <memory>

namespace olaaf {

struct ImportOptions {
  ReadOptions read;
  bool mergeStereo = true;      // ".L" + ".R" tracks -> one stereo track
  bool fades = true;            // import fades and crossfades
  bool clipGain = true;         // import clip gain as take volume
  bool copyToSubdir = true;     // copy WAVs into <dest>/<subdir>; no = reference in place
  std::string destDir;          // usually the REAPER project folder
  std::string subdir;           // default: AAF name + "_Audio"
  std::vector<std::string> searchFolders; // extra places to look for linked media
  double tcOffset = 0.0;        // seconds added to every position
  std::string relativeTo;       // write media paths relative to this folder when they are inside it
};

struct MediaReport {
  int found = 0, missing = 0, copied = 0, converted = 0, extracted = 0, interleaved = 0;
  std::vector<std::string> missingNames;
  std::string logPath; // "olAAF import log.txt" in the audio folder
};

using Progress = std::function<bool(double fraction, const std::string &what)>; // return false to cancel

class Importer {
public:
  bool load(const std::string &aafPath, const ImportOptions &opt, std::string *error);
  Session &session() { return s_; }

  // Locates linked media (search folders + AAF folder). Cheap, no file writes. Call before preview.
  void locateMedia(const ImportOptions &opt);

  // Merges .L/.R track pairs (in place). Returns how many pairs were merged.
  int mergeStereoTracks();

  // Copies / converts / extracts media into the destination and assigns each clip its file.
  bool prepareMedia(const ImportOptions &opt, MediaReport *rep, const Progress &progress);

  // REAPER state chunks, one per imported track ("<TRACK ... >")
  std::vector<std::string> trackChunks(const ImportOptions &opt) const;

  // A complete .rpp project (for the command line tool and REAPER's project import)
  std::string projectRpp(const ImportOptions &opt) const;

private:
  Session s_;
  std::unique_ptr<AafFile> aaf_;
};

std::string defaultSubdir(const Session &s);

} // namespace olaaf
