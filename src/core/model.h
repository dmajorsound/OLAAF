// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The session model: what olAAF reads from an AAF, independent of libAAF and REAPER.
// All times are in seconds on the absolute timecode timeline (01:00:00:00 = 3600.0).

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace olaaf {

// REAPER fade shapes (C_FADEINSHAPE): 0 linear, 1 fast start, 2 fast end,
// 3 fast start steep, 4 fast end steep, 5 slow start/end, 6 slow start/end steep
struct Fade {
  double len = 0.0;
  int shape = 0;
};

enum class MediaKind { Wav, Aiff, Mxf, Embedded, Unknown };

// One audio file referenced by the AAF (one essence)
struct Media {
  std::string name;          // name shown in the editor
  std::string uniqueName;    // unique, safe for file names
  std::string originalUri;   // as written in the AAF (file:///...)
  std::string foundPath;     // where we found it on disk (empty = missing)
  std::string outPath;       // file REAPER will use (after copy / conversion)
  MediaKind kind = MediaKind::Unknown;
  int channels = 1;
  int sampleRate = 48000;
  int bits = 24;
  int64_t length = 0;        // samples per channel
  int64_t timeReference = 0; // samples since midnight (BWF), from the source mob
  void *aafEssence = nullptr; // aafiAudioEssenceFile*, used for embedded extraction
};

// Which media (and which channel of it) feeds one channel of a clip
struct Source {
  int media = -1;   // index into Session::media
  int channel = 0;  // 0 = all channels of the file, else 1-based channel
};

struct GainPoint {
  double time;  // seconds from clip start
  double gain;  // linear
};

struct Clip {
  std::string name;
  double pos = 0.0;       // timeline position (s)
  double len = 0.0;       // length (s)
  double srcOffset = 0.0; // start inside the source file (s)
  double gain = 1.0;      // linear clip gain
  bool mute = false;
  Fade fadeIn, fadeOut;
  std::vector<Source> sources;   // one per clip channel
  std::vector<GainPoint> gainEnv; // clip gain automation (optional)
  std::string outPath;     // file used by REAPER for this clip (set by media stage)
  int chanMode = 0;        // REAPER take channel mode (0 normal, 3 left, 4 right)
  int id = 0;              // unique within the session (selection in the dialog)
  bool excluded = false;   // removed from the import in the dialog
};

struct VideoClip {
  std::string name;
  std::string originalUri;
  std::string foundPath;
  double pos = 0.0, len = 0.0, srcOffset = 0.0;
};

struct Marker {
  double pos = 0.0, len = 0.0;
  std::string name, comment;
  uint32_t color = 0; // 0xRRGGBB, 0 = default
};

struct Track {
  std::string name;
  int channels = 1;
  std::vector<Clip> clips;
  bool import = true;
  uint32_t color = 0;     // 0xRRGGBB, 0 = no color
  double volume = 1.0;    // constant track fader gain from the AAF
  double pan = 0.0;       // -1 left .. +1 right
  std::vector<GainPoint> volEnv; // track volume automation (absolute time, linear gain)
  std::vector<GainPoint> panEnv; // track pan automation (absolute time, -1..+1)
  std::string mergedFrom; // "SFX 1.L + SFX 1.R" when merged
  std::string group;      // folder it goes into: DIALOG, FOLEY, FX, ATMO, MUSIC, OTHER
};

struct Session {
  std::string aafPath;
  std::string name;
  std::string company, product, version;
  double fps = 25.0;
  bool dropFrame = false;
  double start = 0.0;      // composition start (s)
  double end = 0.0;        // composition end (s)
  int sampleRate = 48000;
  int bits = 24;
  std::vector<Track> tracks;
  std::vector<Media> media;
  std::vector<VideoClip> video;
  std::vector<Marker> markers;
  std::vector<std::string> warnings;

  double firstClip() const;
};

// timecode helpers
std::string formatTC(double seconds, double fps);
bool parseTC(const std::string &s, double fps, double *seconds); // "HH:MM:SS:FF"

} // namespace olaaf
