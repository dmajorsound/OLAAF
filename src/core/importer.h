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
  bool video = true;            // video reference from the AAF on its own track
  bool markers = true;          // AAF markers as REAPER markers
  bool folders = true;          // group tracks into folder tracks (DIALOG, FOLEY, FX, ATMO, MUSIC)
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

// one REAPER track to create
struct TrackChunk {
  std::string text;     // "<TRACK ... >"
  int folderDepth = 0;  // REAPER I_FOLDERDEPTH: 1 opens a folder, -1 closes it
  uint32_t color = 0;   // 0xRRGGBB
};

// folder a track belongs in, from its name ("Boom 1" -> DIALOG, "ATMO Stereo 2" -> ATMO)
std::string trackGroup(const std::string &trackName);
uint32_t groupColor(const std::string &group);

using Progress = std::function<bool(double fraction, const std::string &what)>; // return false to cancel

class Importer {
public:
  bool load(const std::string &aafPath, const ImportOptions &opt, std::string *error);
  Session &session() { return s_; }

  // Locates linked media (search folders + AAF folder). Cheap, no file writes. Call before preview.
  void locateMedia(const ImportOptions &opt);

  // Merges .L/.R track pairs (in place). Returns how many pairs were merged.
  int mergeStereoTracks();

  // Switches the .L/.R merge on or off after loading (keeps the unmerged tracks to go back to).
  // Event edits (removed / moved clips) are reset by this.
  int setMergeStereo(bool on);

  // Event edits from the dialog
  void setExcluded(const std::vector<int> &clipIds, bool excluded);
  // Moves clips to another track; with `withAutomation` the track volume/pan automation under each
  // clip goes along (it replaces the target's automation over the clip's time range).
  void moveClips(const std::vector<int> &clipIds, int targetTrack, bool withAutomation);
  Clip *findClip(int id, int *track = nullptr);

  // Copies / converts / extracts media into the destination and assigns each clip its file.
  bool prepareMedia(const ImportOptions &opt, MediaReport *rep, const Progress &progress);

  // REAPER state chunks, one per imported track ("<TRACK ... >")
  std::vector<TrackChunk> trackChunks(const ImportOptions &opt) const;

  // A complete .rpp project (for the command line tool and REAPER's project import)
  std::string projectRpp(const ImportOptions &opt) const;

private:
  Session s_;
  std::unique_ptr<AafFile> aaf_;
  std::vector<Track> unmerged_;
  bool haveUnmerged_ = false;
};

std::string defaultSubdir(const Session &s);

} // namespace olaaf
