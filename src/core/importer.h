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
  bool clipGainToTrack = false; // ... or fold clip gain and its automation into the track volume envelope
  bool overwriteEnvelopes = false; // tracks put on existing REAPER tracks: replace their volume / pan
                                   // envelopes over the AAF's time range with the AAF's (off: leave them)
  bool showAutomation = false;  // dialog view: draw volume / pan automation in the lanes (no effect on the import)
  bool video = true;            // video reference from the AAF on its own track
  bool markers = true;          // AAF markers as REAPER markers
  bool folders = true;          // create the folder tracks in Session::folders
  bool copyToSubdir = false;    // copy WAVs into <dest>/<subdir>; no = reference in place
  std::string destDir;          // usually the REAPER project folder
  std::string subdir;           // default: AAF name + "_Audio"
  std::vector<std::string> searchFolders; // extra places to look for linked media
  double tcOffset = 0.0;        // seconds added to every position
  std::string relativeTo;       // write media paths relative to this folder when they are inside it
  std::string trackTemplate;    // a REAPER .RTrackTemplate added on import ("" = none)
};

struct MediaReport {
  int found = 0, missing = 0, copied = 0, converted = 0, extracted = 0, interleaved = 0;
  std::vector<std::string> missingNames;
  std::string logPath; // "OLAAF import log.txt" in the audio folder
};

// one REAPER track to create
struct TrackChunk {
  std::string text;     // "<TRACK ... >"
  int folderDepth = 0;  // REAPER I_FOLDERDEPTH: 1 opens a folder, -1 closes it
  uint32_t color = 0;   // 0xRRGGBB
  bool videoFromFile = false; // the VIDEO track: take each item's length from its file
  std::string targetGuid;     // set: no new track - put `items` on this existing REAPER track
  std::vector<std::string> items; // the "<ITEM ... >" chunks of the track
  std::vector<GainPoint> volEnv, panEnv; // absolute times (offset applied); pan in REAPER's direction (+1 = left)
  std::string volEnvChunk, panEnvChunk;  // the same as "<VOLENV2 ... >" / "<PANENV2 ... >" blocks
  double rangeStart = 0, rangeEnd = 0;   // where the track's events are (offset applied)
  std::string folderKey; // the folder (Session::folders key) this track is in, or opens
  bool isFolder = false; // the folder track itself
};

// Sorting tracks into folders by their names: each rule is a folder with the words that send a
// track there ("Boom 1" -> DIA, "ATMO Stereo 2" -> ATMO). Editable in the dialog, remembered.
struct SortRule {
  std::string name;
  uint32_t color;
  std::vector<std::string> words;
};
std::vector<SortRule> defaultSortRules(); // DIA, FX, MUS, ATMO
void setSortRules(const std::vector<SortRule> &rules);
const std::vector<SortRule> &sortRules();
std::string sortRulesToText(const std::vector<SortRule> &rules);
std::vector<SortRule> sortRulesFromText(const std::string &text);
std::vector<std::string> splitWords(const std::string &text);
// folder a track belongs in, from its name; "OTHER" when no rule matches
std::string trackGroup(const std::string &trackName);
uint32_t groupColor(const std::string &group);

using Progress = std::function<bool(double fraction, const std::string &what)>; // return false to cancel

class Importer {
public:
  bool load(const std::string &aafPath, const ImportOptions &opt, std::string *error);
  Session &session() { return s_; }
  // reels: the combined session of all AAFs (after their media is ready) replaces this one
  void adoptSession(Session s) { s_ = std::move(s); haveUnmerged_ = false; }

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
  // Appends an empty track (same channel count and folder as `like`) and returns its index.
  int addTrack(int like);
  // Deletes a track added in the dialog; its events go back to the AAF tracks they came from.
  // AAF tracks cannot be deleted (returns false).
  bool removeCreatedTrack(int track, bool withAutomation);
  // Empty means nothing can be lost by deleting it: no events on it (not even removed ones), no
  // events that came from it and go back to it on Reset, no volume or pan automation.
  bool trackIsEmpty(int track) const;
  std::vector<int> emptyTracks() const;
  // deletes the empty tracks (each checked again); their names in `names`; returns how many
  int removeEmptyTracks(std::vector<std::string> *names);
  // Puts the tracks in a new order: newOrder lists every track index once (new position -> old index).
  void reorderTracks(const std::vector<int> &newOrder);
  // Track color as imported: its own color, otherwise the color of its folder (when folders are on)
  uint32_t trackColor(const Track &t, const ImportOptions &opt) const;
  // Track volume as imported at absolute time t (track automation x clip gain when folded into the track).
  double trackVolumeAt(const Track &t, double time, const ImportOptions &opt) const;
  // The track volume envelope as written: the track's own automation, with clip gain folded in when asked.
  std::vector<GainPoint> trackVolumeEnvelope(const Track &t, const ImportOptions &opt) const;

  // Folder tracks. Tracks are sorted into DIALOG, FOLEY, FX, ATMO, MUSIC and OTHER by their names
  // (only when that gives at least two folders); the dialog can rename, add and remove folders.
  void assignDefaultFolders();
  std::string addFolder(const std::string &name); // returns the new key
  void removeFolder(const std::string &key);      // its tracks end up outside any folder

  // Copies / converts / extracts media into the destination and assigns each clip its file.
  bool prepareMedia(const ImportOptions &opt, MediaReport *rep, const Progress &progress);

  // REAPER state chunks, one per imported track ("<TRACK ... >")
  std::vector<TrackChunk> trackChunks(const ImportOptions &opt) const;

  // A complete .rpp project (for the command line tool and REAPER's project import)
  std::string projectRpp(const ImportOptions &opt) const;

private:
  void assignUids();
  Session s_;
  std::unique_ptr<AafFile> aaf_;
  std::vector<Track> unmerged_;
  bool haveUnmerged_ = false;
};

std::string defaultSubdir(const Session &s);

} // namespace olaaf
