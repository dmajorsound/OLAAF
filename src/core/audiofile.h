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

// where the samples of an MXF lie when they are not one block: frame-wrapped essence is many
// KLV packets, AES3 essence has its own sample layout
struct PcmSeg {
  uint64_t offset = 0;    // file offset of the packet's value
  uint64_t bytes = 0;
  int64_t firstFrame = 0; // the first sample frame in it
  int64_t frames = 0;
};

struct PcmInfo {
  int channels = 0;
  int rate = 0;
  int bits = 0;
  bool isFloat = false;
  bool bigEndian = false;      // AIFF
  uint64_t dataOffset = 0;     // byte offset of the first sample
  uint64_t dataBytes = 0;
  int64_t timeReference = -1;  // BWF / MXF timecode in samples since midnight, -1 = none
  std::vector<PcmSeg> segs;    // MXF: the essence packets (empty: one block at dataOffset)
  bool aes3 = false;           // MXF AES3 essence (SMPTE 331M): decoded to 24 bit by PcmFile
  int aesChannels = 8;         // channel slots per AES3 sample
  std::string error;
  int64_t frames() const {
    if (!segs.empty()) return segs.back().firstFrame + segs.back().frames;
    int ba = channels * (bits / 8);
    return ba > 0 ? (int64_t)(dataBytes / (uint64_t)ba) : 0;
  }
};

bool readWavInfo(const std::string &path, PcmInfo *out);
bool readAiffInfo(const std::string &path, PcmInfo *out);
bool readMxfInfo(const std::string &path, PcmInfo *out);

// Reads sample frames of a WAV / AIFF / MXF file, whatever the MXF layout (one block, frame-wrapped
// packets, AES3). The bytes come in the file's sample format (bits, byte order as in the PcmInfo).
class PcmFile {
public:
  PcmFile() = default;
  PcmFile(const PcmFile &) = delete;
  PcmFile &operator=(const PcmFile &) = delete;
  ~PcmFile() { close(); }
  bool open(const std::string &path, const PcmInfo &info);
  void close();
  bool isOpen() const { return f_ != nullptr; }
  const PcmInfo &info() const { return in_; }
  int frameBytes() const { return in_.channels * (in_.bits / 8); }
  // up to n frames from `frame` into out (n * frameBytes() bytes); returns the frames read
  int64_t read(int64_t frame, int64_t n, unsigned char *out);

private:
  FILE *f_ = nullptr;
  PcmInfo in_;
  size_t seg_ = 0; // the packet read last (reading goes forward)
  std::vector<unsigned char> tmp_;
};
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
