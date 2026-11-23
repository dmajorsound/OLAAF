// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Small, dependency-free readers and writers for the PCM formats olAAF handles:
// WAV/BWF (read + write), AIFF/AIFC (read), MXF OP-Atom/OP1a with PCM (read).

#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace olaaf {

struct PcmInfo {
  int channels = 0;
  int rate = 0;
  int bits = 0;
  bool isFloat = false;
  bool bigEndian = false;      // AIFF
  uint64_t dataOffset = 0;     // byte offset of the first sample
  uint64_t dataBytes = 0;
  int64_t timeReference = -1;  // BWF / MXF timecode in samples since midnight, -1 = none
  std::string error;
  int64_t frames() const {
    int ba = channels * (bits / 8);
    return ba > 0 ? (int64_t)(dataBytes / (uint64_t)ba) : 0;
  }
};

bool readWavInfo(const std::string &path, PcmInfo *out);
bool readAiffInfo(const std::string &path, PcmInfo *out);
bool readMxfInfo(const std::string &path, PcmInfo *out);
// Start timecode of a video file (MOV / MP4 timecode track, MXF timecode), in seconds; false if it has none.
bool videoStartTimecode(const std::string &path, double *seconds);

// Streams PCM from any of the readers above into a little-endian Broadcast WAV.
// Lossless: samples are copied (byte order fixed for AIFF), nothing is resampled.
bool convertToBwf(const std::string &src, const PcmInfo &in, const std::string &dst,
                  int64_t timeReference, const std::string &description, std::string *error);

// Interleaves mono sources (same rate and bit depth) into one multichannel BWF.
// Shorter sources are padded with silence.
bool interleaveToBwf(const std::vector<std::string> &srcs, const std::vector<PcmInfo> &ins,
                     const std::string &dst, int64_t timeReference, const std::string &description,
                     std::string *error);

FILE *openUtf8(const std::string &path, const char *mode);

} // namespace olaaf
