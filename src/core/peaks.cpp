// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "peaks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audiofile.h"
#include "model.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

namespace olaaf {

static const int kBlock = 256;  // frames per fine peak
static const int kCoarse = 64;  // fine peaks per coarse peak

std::vector<PeakCache::Job> PeakCache::jobsFor(const Session &s) {
  std::vector<Job> jobs;
  for (auto &m : s.media) {
    Job j;
    if (!m.foundPath.empty() && (m.kind == MediaKind::Wav || m.kind == MediaKind::Aiff || m.kind == MediaKind::Mxf)) {
      j.path = m.foundPath;
      j.kind = m.kind == MediaKind::Aiff ? 1 : m.kind == MediaKind::Mxf ? 2 : 0;
    }
    jobs.push_back(j);
  }
  return jobs;
}

void PeakCache::start(const Session &s) {
  stop();
  auto jobs = jobsFor(s);
  {
    std::lock_guard<std::mutex> l(mu_);
    peaks_.assign(jobs.size(), nullptr);
  }
  stop_ = false;
  busy_ = true;
  th_ = std::thread([this, jobs]() { run(jobs); });
}

void PeakCache::runNow(const Session &s) {
  stop();
  auto jobs = jobsFor(s);
  {
    std::lock_guard<std::mutex> l(mu_);
    peaks_.assign(jobs.size(), nullptr);
  }
  stop_ = false;
  run(jobs);
}

void PeakCache::stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  busy_ = false;
}

void PeakCache::run(std::vector<Job> jobs) {
  busy_ = true;
  int n = 0;
  for (auto &j : jobs) n += !j.path.empty();
  total_ = n;
  done_ = 0;
  // the same file can feed several media entries: read it once
  for (size_t i = 0; i < jobs.size() && !stop_; i++) {
    if (jobs[i].path.empty()) continue;
    std::shared_ptr<Peaks> p;
    for (size_t k = 0; k < i; k++)
      if (jobs[k].path == jobs[i].path) {
        std::lock_guard<std::mutex> l(mu_);
        p = peaks_[k];
        break;
      }
    if (!p) p = scan(jobs[i], stop_);
    done_++;
    if (!p) continue;
    {
      std::lock_guard<std::mutex> l(mu_);
      peaks_[i] = p;
    }
    version_++;
  }
  busy_ = false;
  version_++;
}

static inline float sampleAbs(const unsigned char *p, int bits, bool fl, bool be) {
  unsigned char b[8];
  int n = bits / 8;
  if (be)
    for (int i = 0; i < n; i++) b[i] = p[n - 1 - i];
  else
    memcpy(b, p, n);
  if (fl) {
    if (bits == 32) {
      float f;
      memcpy(&f, b, 4);
      return std::fabs(f);
    }
    double d;
    memcpy(&d, b, 8);
    return (float)std::fabs(d);
  }
  switch (bits) {
  case 8: return std::fabs((b[0] - 128) / 128.0f); // WAV 8 bit is unsigned
  case 16: return std::fabs((int16_t)(b[0] | b[1] << 8) / 32768.0f);
  case 24: {
    int32_t v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8;
    return std::fabs(v / 8388608.0f);
  }
  case 32: {
    int32_t v = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    return std::fabs(v / 2147483648.0f);
  }
  }
  return 0;
}

std::shared_ptr<PeakCache::Peaks> PeakCache::scan(const Job &j, const std::atomic<bool> &stop) {
  PcmInfo in;
  bool ok = j.kind == 1 ? readAiffInfo(j.path, &in) : j.kind == 2 ? readMxfInfo(j.path, &in) : readWavInfo(j.path, &in);
  if (!ok || in.channels <= 0 || in.channels > 64 || in.rate <= 0) return nullptr;
  int bps = in.bits / 8;
  if (bps < 1 || bps > 8) return nullptr;
  if (j.kind == 1 && in.bits == 8) in.bits = 8; // AIFF 8 bit is signed: handled below
  FILE *f = openUtf8(j.path, "rb");
  if (!f) return nullptr;
  if (fseek64(f, (int64_t)in.dataOffset, SEEK_SET)) {
    fclose(f);
    return nullptr;
  }
  auto p = std::make_shared<Peaks>();
  p->channels = in.channels;
  p->rate = in.rate;
  int64_t frames = in.frames();
  p->blocks = (size_t)((frames + kBlock - 1) / kBlock);
  p->fine.assign(p->blocks * in.channels, 0);
  int fb = bps * in.channels;
  std::vector<unsigned char> buf((size_t)fb * kBlock * 256);
  std::vector<float> mx(in.channels);
  int64_t done = 0;
  size_t blk = 0;
  while (done < frames && !stop) {
    int64_t want = std::min<int64_t>(frames - done, kBlock * 256);
    size_t r = fread(buf.data(), 1, (size_t)(want * fb), f);
    int64_t got = (int64_t)r / fb;
    if (got <= 0) break;
    for (int64_t a = 0; a < got; a += kBlock) {
      std::fill(mx.begin(), mx.end(), 0.0f);
      int64_t e = std::min<int64_t>(got, a + kBlock);
      for (int64_t fr = a; fr < e; fr++) {
        const unsigned char *s = buf.data() + fr * fb;
        for (int c = 0; c < in.channels; c++) {
          float v = (in.bits == 8 && j.kind == 1) ? std::fabs((int8_t)s[c] / 128.0f)
                                                  : sampleAbs(s + c * bps, in.bits, in.isFloat, in.bigEndian);
          if (v > mx[c]) mx[c] = v;
        }
      }
      if (blk < p->blocks)
        for (int c = 0; c < in.channels; c++)
          p->fine[blk * in.channels + c] = (unsigned char)std::lround(std::min(1.0f, mx[c]) * 255);
      blk++;
    }
    done += got;
  }
  fclose(f);
  if (stop) return nullptr;
  p->coarseBlocks = (p->blocks + kCoarse - 1) / kCoarse;
  p->coarse.assign(p->coarseBlocks * in.channels, 0);
  for (size_t b = 0; b < p->blocks; b++)
    for (int c = 0; c < in.channels; c++) {
      unsigned char &d = p->coarse[(b / kCoarse) * in.channels + c];
      d = std::max(d, p->fine[b * in.channels + c]);
    }
  return p;
}

bool PcmReader::open(const std::string &path) {
  if (f_ && path == path_) return true;
  close();
  PcmInfo in;
  std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
  for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
  bool ok = ext == ".mxf"   ? readMxfInfo(path, &in)
            : (ext == "aiff" || ext == ".aif" || ext == "aifc") ? readAiffInfo(path, &in)
                                                                 : readWavInfo(path, &in);
  if (!ok || in.channels <= 0 || in.rate <= 0 || in.bits < 8 || in.bits > 64) return false;
  FILE *f = openUtf8(path, "rb");
  if (!f) return false;
  f_ = f;
  path_ = path;
  channels_ = in.channels;
  rate_ = in.rate;
  bits_ = in.bits;
  float_ = in.isFloat;
  big_ = in.bigEndian;
  aiff8_ = in.bigEndian && in.bits == 8;
  offset_ = in.dataOffset;
  frames_ = in.frames();
  return true;
}

void PcmReader::close() {
  if (f_) fclose((FILE *)f_);
  f_ = nullptr;
  path_.clear();
}

void PcmReader::read(double time, int channel, int n, std::vector<float> &out) {
  out.assign((size_t)std::max(0, n), 0.0f);
  if (!f_ || n <= 0 || channel < 0 || channel >= channels_) return;
  int bps = bits_ / 8, fb = bps * channels_;
  int64_t a = (int64_t)std::floor(time * rate_);
  int64_t s0 = std::max<int64_t>(0, a), s1 = std::min<int64_t>(frames_, a + n);
  if (s1 <= s0) return;
  buf_.resize((size_t)((s1 - s0) * fb));
  if (fseek64((FILE *)f_, (int64_t)offset_ + s0 * fb, SEEK_SET)) return;
  size_t got = fread(buf_.data(), 1, buf_.size(), (FILE *)f_) / (size_t)fb;
  for (size_t i = 0; i < got; i++) {
    const unsigned char *p = buf_.data() + i * fb + channel * bps;
    unsigned char b[8];
    if (big_)
      for (int k = 0; k < bps; k++) b[k] = p[bps - 1 - k];
    else
      memcpy(b, p, (size_t)bps);
    float v = 0;
    if (float_) {
      if (bits_ == 32) memcpy(&v, b, 4);
      else {
        double d;
        memcpy(&d, b, 8);
        v = (float)d;
      }
    } else if (bits_ == 8) v = aiff8_ ? (int8_t)b[0] / 128.0f : (b[0] - 128) / 128.0f;
    else if (bits_ == 16) v = (int16_t)(b[0] | b[1] << 8) / 32768.0f;
    else if (bits_ == 24) v = ((int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8) / 8388608.0f;
    else if (bits_ == 32)
      v = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24) / 2147483648.0f;
    out[(size_t)(s0 - a) + i] = v;
  }
}

int PeakCache::channels(int media) const {
  std::lock_guard<std::mutex> l(mu_);
  if (media < 0 || media >= (int)peaks_.size() || !peaks_[media]) return 0;
  return peaks_[media]->channels;
}

float PeakCache::peak(int media, int channel, double t0, double t1) const {
  std::shared_ptr<Peaks> p;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (media < 0 || media >= (int)peaks_.size()) return -1;
    p = peaks_[media];
  }
  if (!p || channel < 0 || channel >= p->channels) return -1;
  double f0 = t0 * p->rate / kBlock, f1 = t1 * p->rate / kBlock;
  if (f1 < 0 || f0 >= (double)p->blocks) return 0;
  bool coarse = f1 - f0 > kCoarse * 2;
  const std::vector<unsigned char> &v = coarse ? p->coarse : p->fine;
  size_t n = coarse ? p->coarseBlocks : p->blocks;
  if (coarse) {
    f0 /= kCoarse;
    f1 /= kCoarse;
  }
  size_t a = (size_t)std::max(0.0, std::floor(f0)), b = (size_t)std::max(0.0, std::ceil(f1));
  b = std::min(std::max(b, a + 1), n);
  unsigned char m = 0;
  for (size_t i = a; i < b; i++) m = std::max(m, v[i * p->channels + channel]);
  return m / 255.0f;
}

} // namespace olaaf
