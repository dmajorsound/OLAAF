// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "peaks.h"
#include "util.h"

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
      // the file found decides (a located file can differ from what the AAF named)
      std::string e = lower(fileExt(m.foundPath));
      j.kind = e == "mxf" ? 2 : (e == "aif" || e == "aiff" || e == "aifc") ? 1 : e == "wav" || e == "bwf" ? 0
               : m.kind == MediaKind::Aiff ? 1 : m.kind == MediaKind::Mxf ? 2 : 0;
    }
    jobs.push_back(j);
  }
  return jobs;
}

void PeakCache::start(const Session &s) {
  stop();
  auto jobs = jobsFor(s);
  prefill(jobs);
  stop_ = false;
  busy_ = true;
  th_ = std::thread([this, jobs]() { run(jobs); });
}

void PeakCache::runNow(const Session &s) {
  stop();
  auto jobs = jobsFor(s);
  prefill(jobs);
  stop_ = false;
  run(jobs);
}

// ---- waveforms on disk: one small file per media file, named by a hash of its path ----
// It is used only while the media file has the same size and date as when it was read.
static std::string g_diskDir;
static std::mutex g_diskMu;
void PeakCache::setDiskFolder(const std::string &dir) {
  {
    std::lock_guard<std::mutex> l(g_diskMu);
    g_diskDir = dir;
  }
  if (!dir.empty()) trimFolder(dir, 2000ull << 20); // keeps the newest 2 GB
}

static uint64_t fnv64(const std::string &s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

static const char kPeakMagic[8] = {'O', 'L', 'P', 'K', '0', '0', '0', '1'};

static std::string diskName(const std::string &path) {
  std::lock_guard<std::mutex> l(g_diskMu);
  if (g_diskDir.empty()) return "";
  return joinPath(g_diskDir, strprintf("%016llx.olpk", (unsigned long long)fnv64(path)));
}

std::shared_ptr<PeakCache::Peaks> PeakCache::loadDisk(const std::string &path) {
  std::string name = diskName(path);
  uint64_t size = 0;
  int64_t changed = 0;
  if (name.empty() || !fileStamp(path, &size, &changed)) return nullptr;
  FILE *f = openUtf8(name, "rb");
  if (!f) return nullptr;
  auto p = std::make_shared<Peaks>();
  bool ok = false;
  do {
    char magic[8];
    uint64_t sz, fineN, coarseN, blocks, cblocks;
    int64_t ch;
    int32_t channels, rate;
    uint32_t plen;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, kPeakMagic, 8)) break;
    if (fread(&plen, 4, 1, f) != 1 || plen > 65536) break;
    std::string stored(plen, '\0');
    if (plen && fread(&stored[0], 1, plen, f) != plen) break;
    if (stored != path) break; // another file with the same hash
    if (fread(&sz, 8, 1, f) != 1 || fread(&ch, 8, 1, f) != 1 || sz != size || ch != changed) break;
    if (fread(&channels, 4, 1, f) != 1 || fread(&rate, 4, 1, f) != 1) break;
    if (fread(&blocks, 8, 1, f) != 1 || fread(&cblocks, 8, 1, f) != 1) break;
    if (fread(&fineN, 8, 1, f) != 1 || fread(&coarseN, 8, 1, f) != 1) break;
    if (channels <= 0 || channels > 64 || fineN != blocks * (uint64_t)channels || coarseN != cblocks * (uint64_t)channels ||
        fineN > (1ull << 32))
      break;
    p->channels = channels;
    p->rate = rate;
    p->blocks = (size_t)blocks;
    p->coarseBlocks = (size_t)cblocks;
    p->fine.resize((size_t)fineN);
    p->coarse.resize((size_t)coarseN);
    if (fineN && fread(p->fine.data(), 1, (size_t)fineN, f) != fineN) break;
    if (coarseN && fread(p->coarse.data(), 1, (size_t)coarseN, f) != coarseN) break;
    ok = true;
  } while (false);
  fclose(f);
  return ok ? p : nullptr;
}

void PeakCache::saveDisk(const std::string &path, const Peaks &p) {
  std::string name = diskName(path);
  uint64_t size = 0;
  int64_t changed = 0;
  if (name.empty() || !fileStamp(path, &size, &changed)) return;
  makeDirs(dirName(name));
  std::string tmp = name + ".part";
  FILE *f = openUtf8(tmp, "wb");
  if (!f) return;
  uint32_t plen = (uint32_t)path.size();
  int32_t channels = p.channels, rate = p.rate;
  uint64_t blocks = p.blocks, cblocks = p.coarseBlocks, fineN = p.fine.size(), coarseN = p.coarse.size();
  bool ok = fwrite(kPeakMagic, 1, 8, f) == 8 && fwrite(&plen, 4, 1, f) == 1 &&
            (!plen || fwrite(path.data(), 1, plen, f) == plen) && fwrite(&size, 8, 1, f) == 1 &&
            fwrite(&changed, 8, 1, f) == 1 && fwrite(&channels, 4, 1, f) == 1 && fwrite(&rate, 4, 1, f) == 1 &&
            fwrite(&blocks, 8, 1, f) == 1 && fwrite(&cblocks, 8, 1, f) == 1 && fwrite(&fineN, 8, 1, f) == 1 &&
            fwrite(&coarseN, 8, 1, f) == 1 && (!fineN || fwrite(p.fine.data(), 1, fineN, f) == fineN) &&
            (!coarseN || fwrite(p.coarse.data(), 1, coarseN, f) == coarseN);
  fclose(f);
  if (ok) ok = renamePath(tmp, name);
  if (!ok) removePath(tmp);
}

void PeakCache::prefill(const std::vector<Job> &jobs) {
  std::lock_guard<std::mutex> l(mu_);
  peaks_.assign(jobs.size(), nullptr);
  why_.assign(jobs.size(), std::string());
  for (size_t i = 0; i < jobs.size(); i++) {
    if (jobs[i].path.empty()) continue;
    auto p = byPath_.find(jobs[i].path);
    if (p != byPath_.end()) peaks_[i] = p->second;
    auto w = whyByPath_.find(jobs[i].path);
    if (w != whyByPath_.end()) why_[i] = w->second;
  }
}

void PeakCache::stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  busy_ = false;
}

void PeakCache::run(std::vector<Job> jobs) {
  busy_ = true;
  int n = 0;
  {
    std::lock_guard<std::mutex> l(mu_); // only the files not read before count
    for (size_t i = 0; i < jobs.size(); i++)
      if (!jobs[i].path.empty() && !peaks_[i] && why_[i].empty()) n++;
  }
  total_ = n;
  done_ = 0;
  // the same file can feed several media entries: read it once
  for (size_t i = 0; i < jobs.size() && !stop_; i++) {
    if (jobs[i].path.empty()) continue;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (peaks_[i] || !why_[i].empty()) continue; // read before (another reel, or the last time this one was shown)
    }
    std::shared_ptr<Peaks> p;
    for (size_t k = 0; k < i; k++)
      if (jobs[k].path == jobs[i].path) {
        std::lock_guard<std::mutex> l(mu_);
        p = peaks_[k];
        break;
      }
    std::string why;
    bool fromDisk = false;
    if (!p) fromDisk = (p = loadDisk(jobs[i].path)) != nullptr; // read in an earlier import
    if (!p) {
      p = scan(jobs[i], stop_, &why);
      if (p && !stop_ && !fromDisk) saveDisk(jobs[i].path, *p);
    }
    if (!p && !stop_) {
      std::lock_guard<std::mutex> l(mu_);
      if (i < why_.size()) why_[i] = why.empty() ? "the file can't be read" : why;
      whyByPath_[jobs[i].path] = why_[i];
    }
    done_++;
    if (!p) continue;
    {
      std::lock_guard<std::mutex> l(mu_);
      peaks_[i] = p;
      byPath_[jobs[i].path] = p;
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

std::shared_ptr<PeakCache::Peaks> PeakCache::scan(const Job &j, const std::atomic<bool> &stop, std::string *why) {
  PcmInfo in;
  bool ok = j.kind == 1 ? readAiffInfo(j.path, &in) : j.kind == 2 ? readMxfInfo(j.path, &in) : readWavInfo(j.path, &in);
  if (!ok) {
    *why = in.error.empty() ? "the file's audio format isn't understood" : in.error;
    return nullptr;
  }
  if (in.channels <= 0 || in.channels > 64 || in.rate <= 0) {
    *why = strprintf("%d channels at %d Hz can't be shown", in.channels, in.rate);
    return nullptr;
  }
  int bps = in.bits / 8;
  if (bps < 1 || bps > 8) {
    *why = strprintf("%d bit audio can't be shown", in.bits);
    return nullptr;
  }
  PcmFile pf; // one block, frame-wrapped packets or AES3: all the same here
  if (!pf.open(j.path, in)) {
    *why = "the file can't be opened";
    return nullptr;
  }
  auto p = std::make_shared<Peaks>();
  p->channels = in.channels;
  p->rate = in.rate;
  int64_t frames = in.frames();
  p->blocks = (size_t)((frames + kBlock - 1) / kBlock);
  p->fine.assign(p->blocks * in.channels, 0);
  int fb = pf.frameBytes();
  std::vector<unsigned char> buf((size_t)fb * kBlock * 256);
  std::vector<float> mx(in.channels);
  int64_t done = 0;
  size_t blk = 0;
  while (done < frames && !stop) {
    int64_t want = std::min<int64_t>(frames - done, kBlock * 256);
    int64_t got = pf.read(done, want, buf.data());
    if (got <= 0) break;
    for (int64_t a = 0; a < got; a += kBlock) {
      std::fill(mx.begin(), mx.end(), 0.0f);
      int64_t e = std::min<int64_t>(got, a + kBlock);
      for (int64_t fr = a; fr < e; fr++) {
        const unsigned char *sm = buf.data() + fr * fb;
        for (int c = 0; c < in.channels; c++) {
          float v = (in.bits == 8 && j.kind == 1) ? std::fabs((int8_t)sm[c] / 128.0f)
                                                  : sampleAbs(sm + c * bps, in.bits, in.isFloat, in.bigEndian);
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
  if (file_.isOpen() && path == path_) return true;
  close();
  PcmInfo in;
  std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
  for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
  bool ok = ext == ".mxf"   ? readMxfInfo(path, &in)
            : (ext == "aiff" || ext == ".aif" || ext == "aifc") ? readAiffInfo(path, &in)
                                                                 : readWavInfo(path, &in);
  if (!ok || in.channels <= 0 || in.rate <= 0 || in.bits < 8 || in.bits > 64) return false;
  if (!file_.open(path, in)) return false;
  path_ = path;
  channels_ = in.channels;
  rate_ = in.rate;
  bits_ = in.bits;
  float_ = in.isFloat;
  big_ = in.bigEndian;
  aiff8_ = in.bigEndian && in.bits == 8;
  frames_ = in.frames();
  return true;
}

void PcmReader::close() {
  file_.close();
  path_.clear();
}

void PcmReader::read(double time, int channel, int n, std::vector<float> &out) {
  out.assign((size_t)std::max(0, n), 0.0f);
  if (!file_.isOpen() || n <= 0 || channel < 0 || channel >= channels_) return;
  int bps = bits_ / 8, fb = bps * channels_;
  int64_t a = (int64_t)std::floor(time * rate_);
  int64_t s0 = std::max<int64_t>(0, a), s1 = std::min<int64_t>(frames_, a + n);
  if (s1 <= s0) return;
  buf_.resize((size_t)((s1 - s0) * fb));
  size_t got = (size_t)file_.read(s0, s1 - s0, buf_.data());
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

std::string PeakCache::why(int media) const {
  std::lock_guard<std::mutex> l(mu_);
  return media >= 0 && media < (int)why_.size() ? why_[(size_t)media] : std::string();
}

int PeakCache::failed() const {
  std::lock_guard<std::mutex> l(mu_);
  int n = 0;
  for (auto &w : why_) n += !w.empty();
  return n;
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
