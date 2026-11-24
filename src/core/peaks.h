// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Waveform overviews for the import dialog. A background thread reads every
// linked PCM file once (WAV, AIFF, MXF) and keeps the absolute peak per block
// of 256 frames, plus a coarser level (x 64) for zoomed-out views.

#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace olaaf {

struct Session;

// Reads short stretches of a PCM file (WAV, AIFF, MXF) as floats, for the stereo meter.
class PcmReader {
public:
  PcmReader() = default;
  PcmReader(const PcmReader &) = delete;
  PcmReader &operator=(const PcmReader &) = delete;
  ~PcmReader() { close(); }
  bool open(const std::string &path);
  void close();
  bool isOpen() const { return f_ != nullptr; }
  int channels() const { return channels_; }
  int rate() const { return rate_; }
  // n frames of one channel (0-based) from `time` seconds; silence outside the file
  void read(double time, int channel, int n, std::vector<float> &out);

private:
  void *f_ = nullptr;
  std::string path_;
  int channels_ = 0, rate_ = 0, bits_ = 0;
  bool float_ = false, big_ = false, aiff8_ = false;
  uint64_t offset_ = 0;
  int64_t frames_ = 0;
  std::vector<unsigned char> buf_;
};

class PeakCache {
public:
  ~PeakCache() { stop(); }
  // Starts reading all found media of the session in the background.
  void start(const Session &s);
  // Same, but on the calling thread (tests).
  void runNow(const Session &s);
  void stop();
  // Bumps every time another file is ready: repaint when it changes.
  int version() const { return version_.load(); }
  bool busy() const { return busy_.load(); }
  // files read so far / files to read (for "Creating waveforms")
  int done() const { return done_.load(); }
  int total() const { return total_.load(); }
  void fakeProgress(int done, int total) { busy_ = true; done_ = done; total_ = total; } // for the layout tests
  // Highest absolute level (0..1) of one channel (0-based) of a media file between two
  // source times in seconds; -1 if this file has no overview (yet).
  float peak(int media, int channel, double t0, double t1) const;
  int channels(int media) const;

private:
  struct Job {
    std::string path;
    int kind = 0; // 0 wav, 1 aiff, 2 mxf
  };
  struct Peaks {
    int channels = 0, rate = 0;
    std::vector<unsigned char> fine, coarse; // interleaved per channel
    size_t blocks = 0, coarseBlocks = 0;
  };
  void run(std::vector<Job> jobs);
  static std::shared_ptr<Peaks> scan(const Job &j, const std::atomic<bool> &stop);
  std::vector<Job> jobsFor(const Session &s);

  mutable std::mutex mu_;
  std::vector<std::shared_ptr<Peaks>> peaks_;
  std::thread th_;
  std::atomic<bool> stop_{false}, busy_{false};
  std::atomic<int> version_{0}, done_{0}, total_{0};
};

} // namespace olaaf
