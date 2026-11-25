// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include <functional>
#include "audiofile.h"
#include "util.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>

#if defined(_WIN32)
#include <windows.h>
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

namespace olaaf {

FILE *openUtf8(const std::string &path, const char *mode) {
#if defined(_WIN32)
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
  wchar_t wm[8] = {0};
  for (int i = 0; mode[i] && i < 7; i++) wm[i] = mode[i];
  return _wfopen(w.c_str(), wm);
#else
  return fopen(path.c_str(), mode);
#endif
}

struct FileCloser {
  void operator()(FILE *f) const { if (f) fclose(f); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

static uint32_t le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t be64(const unsigned char *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// ---------------------------------------------------------------- WAV

bool readWavInfo(const std::string &path, PcmInfo *o) {
  FilePtr f(openUtf8(path, "rb"));
  if (!f) { o->error = "cannot open"; return false; }
  unsigned char h[12];
  if (fread(h, 1, 12, f.get()) != 12) { o->error = "too short"; return false; }
  bool rf64 = !memcmp(h, "RF64", 4) || !memcmp(h, "BW64", 4);
  if ((memcmp(h, "RIFF", 4) && !rf64) || memcmp(h + 8, "WAVE", 4)) { o->error = "not a WAV file"; return false; }
  uint64_t ds64Data = 0;
  bool haveFmt = false, haveData = false;
  for (;;) {
    unsigned char ch[8];
    if (fread(ch, 1, 8, f.get()) != 8) break;
    uint64_t sz = le32(ch + 4);
    int64_t pos = ftell64(f.get());
    if (!memcmp(ch, "ds64", 4)) {
      unsigned char d[28];
      if (fread(d, 1, 28, f.get()) == 28) ds64Data = le64(d + 8);
    } else if (!memcmp(ch, "fmt ", 4)) {
      unsigned char d[40] = {0};
      size_t n = fread(d, 1, sz < 40 ? (size_t)sz : 40, f.get());
      if (n < 16) break;
      uint16_t tag = le16(d);
      o->channels = le16(d + 2);
      o->rate = (int)le32(d + 4);
      o->bits = le16(d + 14);
      if (tag == 0xFFFE && n >= 26) tag = le16(d + 24);
      o->isFloat = tag == 3;
      if (tag != 1 && tag != 3) { o->error = "compressed WAV"; return false; }
      haveFmt = true;
    } else if (!memcmp(ch, "bext", 4)) {
      unsigned char d[346];
      if (sz >= 346 && fread(d, 1, 346, f.get()) == 346) o->timeReference = (int64_t)le64(d + 338);
    } else if (!memcmp(ch, "data", 4)) {
      o->dataOffset = (uint64_t)pos;
      o->dataBytes = (rf64 && sz == 0xFFFFFFFF && ds64Data) ? ds64Data : sz;
      haveData = true;
      if (haveFmt) break;
    }
    if (rf64 && sz == 0xFFFFFFFF && !memcmp(ch, "data", 4)) sz = ds64Data;
    if (fseek64(f.get(), pos + (int64_t)sz + (int64_t)(sz & 1), SEEK_SET)) break;
  }
  if (!haveFmt || !haveData) { o->error = "WAV without fmt or data"; return false; }
  return true;
}

// ---------------------------------------------------------------- AIFF

static double ext80(const unsigned char *p) {
  int e = ((p[0] & 0x7f) << 8) | p[1];
  uint64_t m = be64(p + 2);
  if (!e && !m) return 0;
  double v = std::ldexp((double)m, e - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

bool readAiffInfo(const std::string &path, PcmInfo *o) {
  FilePtr f(openUtf8(path, "rb"));
  if (!f) { o->error = "cannot open"; return false; }
  unsigned char h[12];
  if (fread(h, 1, 12, f.get()) != 12 || memcmp(h, "FORM", 4)) { o->error = "not an AIFF file"; return false; }
  bool aifc = !memcmp(h + 8, "AIFC", 4);
  if (!aifc && memcmp(h + 8, "AIFF", 4)) { o->error = "not an AIFF file"; return false; }
  o->bigEndian = true;
  bool haveComm = false, haveData = false;
  for (;;) {
    unsigned char ch[8];
    if (fread(ch, 1, 8, f.get()) != 8) break;
    uint64_t sz = be32(ch + 4);
    int64_t pos = ftell64(f.get());
    if (!memcmp(ch, "COMM", 4)) {
      unsigned char d[22] = {0};
      if (fread(d, 1, sz < 22 ? (size_t)sz : 22, f.get()) < 18) break;
      o->channels = be16(d);
      o->bits = be16(d + 6);
      o->rate = (int)std::lround(ext80(d + 8));
      if (aifc && sz >= 22) {
        if (!memcmp(d + 18, "sowt", 4)) o->bigEndian = false;
        else if (!memcmp(d + 18, "fl32", 4) || !memcmp(d + 18, "FL32", 4)) o->isFloat = true;
        else if (memcmp(d + 18, "NONE", 4) && memcmp(d + 18, "twos", 4)) { o->error = "compressed AIFF"; return false; }
      }
      haveComm = true;
    } else if (!memcmp(ch, "SSND", 4)) {
      unsigned char d[8];
      if (fread(d, 1, 8, f.get()) != 8) break;
      o->dataOffset = (uint64_t)pos + 8 + be32(d);
      o->dataBytes = sz - 8 - be32(d);
      haveData = true;
    }
    if (fseek64(f.get(), pos + (int64_t)sz + (int64_t)(sz & 1), SEEK_SET)) break;
  }
  if (!haveComm || !haveData) { o->error = "AIFF without COMM or SSND"; return false; }
  o->bits = (o->bits + 7) / 8 * 8;
  return true;
}

// ---------------------------------------------------------------- MXF
// Enough of SMPTE 377 to find the PCM sound essence and its timecode.

static const unsigned char kKey[4] = {0x06, 0x0e, 0x2b, 0x34};

struct Klv {
  unsigned char key[16];
  uint64_t valueOffset;
  uint64_t len;
};

static bool readKlv(FILE *f, Klv *k) {
  int64_t start = ftell64(f);
  if (fread(k->key, 1, 16, f) != 16) return false;
  if (memcmp(k->key, kKey, 4)) {
    // resync: search the next key prefix (some writers leave padding between sets)
    unsigned char buf[4096];
    int64_t base = start + 1;
    for (;;) {
      if (fseek64(f, base, SEEK_SET)) return false;
      size_t n = fread(buf, 1, sizeof(buf), f);
      if (n < 4) return false;
      for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(buf + i, kKey, 4)) {
          fseek64(f, base + (int64_t)i, SEEK_SET);
          return readKlv(f, k);
        }
      base += (int64_t)n - 3;
    }
  }
  int b = fgetc(f);
  if (b < 0) return false;
  uint64_t len = 0;
  if (b & 0x80) {
    int n = b & 0x7f;
    if (n > 8) return false;
    for (int i = 0; i < n; i++) {
      int c = fgetc(f);
      if (c < 0) return false;
      len = (len << 8) | (unsigned)c;
    }
  } else
    len = (uint64_t)b;
  k->valueOffset = (uint64_t)ftell64(f);
  k->len = len;
  return true;
}

bool readMxfInfo(const std::string &path, PcmInfo *o) {
  FilePtr f(openUtf8(path, "rb"));
  if (!f) { o->error = "cannot open"; return false; }
  static const unsigned char primerKey[16] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                                              0x0d, 0x01, 0x02, 0x01, 0x01, 0x05, 0x01, 0x00};
  std::vector<std::pair<uint16_t, std::vector<unsigned char>>> primer;
  int64_t tcFrames = 0, srcStart = 0;
  int tcBase = 25;
  int rateNum = 0, rateDen = 1;
  uint64_t essOffset = 0, essBytes = 0;
  int essCount = 0;
  bool compressed = false;
  bool gaps = false;
  std::vector<PcmSeg> packets;

  auto ulFor = [&](uint16_t tag) -> const unsigned char * {
    for (auto &p : primer)
      if (p.first == tag) return p.second.data();
    return nullptr;
  };
  // match a metadata UL, ignoring the registry version byte
  auto isUL = [](const unsigned char *ul, const char *hex) {
    unsigned char r[16];
    for (int i = 0; i < 16; i++) {
      unsigned v;
      sscanf(hex + 2 * i, "%2x", &v);
      r[i] = (unsigned char)v;
    }
    return ul && !memcmp(ul, r, 7) && !memcmp(ul + 8, r + 8, 7);
  };

  Klv k;
  while (readKlv(f.get(), &k)) {
    if (!memcmp(k.key, primerKey, 16) && k.len >= 8 && k.len < (1u << 24)) {
      std::vector<unsigned char> d((size_t)k.len);
      if (fread(d.data(), 1, d.size(), f.get()) != d.size()) break;
      uint32_t cnt = be32(&d[0]), sz = be32(&d[4]);
      for (uint32_t i = 0; i < cnt && 8 + (i + 1) * sz <= d.size() && sz >= 18; i++) {
        const unsigned char *p = &d[8 + i * sz];
        primer.push_back({be16(p), std::vector<unsigned char>(p + 2, p + 18)});
      }
    } else if (k.key[4] == 0x02 && k.key[5] == 0x53 && k.len < (1u << 20)) {
      // local set; byte 14 of the key is the set type
      std::vector<unsigned char> d((size_t)k.len);
      if (fread(d.data(), 1, d.size(), f.get()) != d.size()) break;
      int setType = k.key[14];
      for (size_t p = 0; p + 4 <= d.size();) {
        uint16_t tag = be16(&d[p]), ln = be16(&d[p + 2]);
        const unsigned char *v = &d[p + 4];
        if (p + 4 + ln > d.size()) break;
        const unsigned char *ul = ulFor(tag);
        if (setType == 0x14) { // TimecodeComponent
          if ((tag == 0x1501 || isUL(ul, "060e2b34010101020702010301050000")) && ln == 8)
            tcFrames = std::max<int64_t>(tcFrames, (int64_t)be64(v));
          if ((tag == 0x1502 || isUL(ul, "060e2b34010101010404010102060000")) && ln == 2) tcBase = be16(v);
        } else if (setType == 0x11) { // SourceClip
          if ((tag == 0x1201 || isUL(ul, "060e2b34010101020702010104000000")) && ln == 8)
            srcStart = std::max<int64_t>(srcStart, (int64_t)be64(v));
        } else {
          if ((tag == 0x3D03 || isUL(ul, "060e2b34010101050402030101010000")) && ln == 8) {
            rateNum = (int)be32(v);
            rateDen = (int)be32(v + 4);
          } else if ((tag == 0x3D07 || isUL(ul, "060e2b34010101050402010104000000")) && ln == 4)
            o->channels = (int)be32(v);
          else if ((tag == 0x3D01 || isUL(ul, "060e2b34010101040402030304000000")) && ln == 4)
            o->bits = (int)be32(v);
        }
        p += 4 + ln;
      }
    } else if (k.key[4] == 0x01 && k.key[5] == 0x02 && k.key[8] == 0x0d && k.key[12] == 0x16) {
      // sound essence element: 0x01/0x02 BWF frame/clip wrapped, 0x03/0x04 AES3 frame/clip wrapped (PCM),
      // anything else (e.g. compressed) is refused
      int type = k.key[14];
      if (type < 0x01 || type > 0x04) compressed = true;
      if (type == 0x03 || type == 0x04) o->aes3 = true;
      if (essCount == 0) {
        essOffset = k.valueOffset;
        essBytes = k.len;
      } else
        gaps = true; // frame-wrapped: more packets follow, each with its own KLV header
      PcmSeg sg;
      sg.offset = k.valueOffset;
      sg.bytes = k.len;
      packets.push_back(sg);
      essCount++;
      if (fseek64(f.get(), (int64_t)(k.valueOffset + k.len), SEEK_SET)) break;
      continue;
    } else {
      if (fseek64(f.get(), (int64_t)(k.valueOffset + k.len), SEEK_SET)) break;
    }
  }
  if (compressed) { o->error = "MXF holds compressed audio"; return false; }
  if (!essCount) { o->error = "no PCM audio found in MXF"; return false; }
  o->rate = rateDen ? (int)std::lround((double)rateNum / rateDen) : 48000;
  if (!o->channels) o->channels = 1;
  if (!o->bits) o->bits = 24;
  o->dataOffset = essOffset;
  o->dataBytes = essBytes;
  if (o->aes3) {
    // SMPTE 331M: per packet a 4 byte header (flags, sample count, valid channels), then per sample
    // 8 channel slots of 4 bytes with the audio in bits 4..27. Read as 24 bit.
    o->bits = 24;
    o->aesChannels = 8;
    if (o->channels > 8) { o->error = "AES3 MXF with more than 8 channels"; return false; }
    int64_t at = 0;
    for (auto &sg : packets) {
      sg.firstFrame = at;
      sg.frames = sg.bytes >= 4 ? (int64_t)((sg.bytes - 4) / (uint64_t)(o->aesChannels * 4)) : 0;
      at += sg.frames;
    }
    o->segs = packets;
  } else if (gaps) { // frame-wrapped PCM: the packets one after the other
    int fb = o->channels * (o->bits / 8);
    if (fb <= 0) { o->error = "MXF audio format not understood"; return false; }
    int64_t at = 0;
    for (auto &sg : packets) {
      sg.firstFrame = at;
      sg.frames = (int64_t)(sg.bytes / (uint64_t)fb);
      at += sg.frames;
    }
    o->segs = packets;
  }
  // tape timecode + where this file starts on the tape (SourceClip edit rate = audio rate for sound tracks)
  o->timeReference = (tcBase > 0 ? tcFrames * o->rate / tcBase : 0) + srcStart;
  return true;
}

// ---------------------------------------------------------------- video timecode

// MXF: the start timecode of the file (TimecodeComponent in the header metadata), in seconds.
static bool mxfStartTimecode(const std::string &path, double *seconds) {
  FilePtr f(openUtf8(path, "rb"));
  if (!f) return false;
  int64_t tcFrames = -1;
  int tcBase = 0;
  Klv k;
  int sets = 0;
  while (readKlv(f.get(), &k)) {
    // the essence begins: the header metadata is behind us
    if (k.key[4] == 0x01 && k.key[5] == 0x02 && k.key[8] == 0x0d && k.key[9] == 0x01 && k.key[10] == 0x03) break;
    if (k.key[4] == 0x02 && k.key[5] == 0x53 && k.key[14] == 0x14 && k.len < (1u << 16)) { // TimecodeComponent
      std::vector<unsigned char> d((size_t)k.len);
      if (fread(d.data(), 1, d.size(), f.get()) != d.size()) break;
      for (size_t p = 0; p + 4 <= d.size();) {
        uint16_t tag = be16(&d[p]), ln = be16(&d[p + 2]);
        if (p + 4 + ln > d.size()) break;
        const unsigned char *v = &d[p + 4];
        if (tag == 0x1501 && ln == 8) tcFrames = std::max<int64_t>(tcFrames, (int64_t)be64(v));
        if (tag == 0x1502 && ln == 2) tcBase = be16(v);
        p += 4 + ln;
      }
      continue;
    }
    if (++sets > 200000) break;
    if (fseek64(f.get(), (int64_t)(k.valueOffset + k.len), SEEK_SET)) break;
  }
  if (tcFrames < 0 || tcBase <= 0) return false;
  *seconds = (double)tcFrames / tcBase;
  return true;
}

// QuickTime / MP4: the first sample of the timecode ('tmcd') track, in seconds.
static bool movStartTimecode(const std::string &path, double *seconds) {
  FilePtr f(openUtf8(path, "rb"));
  if (!f) return false;
  fseek64(f.get(), 0, SEEK_END);
  int64_t fileSize = ftell64(f.get());
  // find moov at the top level
  int64_t pos = 0, moovPos = -1, moovSize = 0;
  while (pos + 8 <= fileSize) {
    unsigned char h[16];
    fseek64(f.get(), pos, SEEK_SET);
    if (fread(h, 1, 8, f.get()) != 8) break;
    uint64_t sz = be32(h);
    int hdr = 8;
    if (sz == 1) {
      if (fread(h + 8, 1, 8, f.get()) != 8) break;
      sz = be64(h + 8);
      hdr = 16;
    } else if (sz == 0)
      sz = (uint64_t)(fileSize - pos);
    if (sz < (uint64_t)hdr) break;
    if (!memcmp(h + 4, "moov", 4)) {
      moovPos = pos + hdr;
      moovSize = (int64_t)sz - hdr;
      break;
    }
    pos += (int64_t)sz;
  }
  if (moovPos < 0 || moovSize <= 0 || moovSize > (64 << 20)) return false;
  std::vector<unsigned char> m((size_t)moovSize);
  fseek64(f.get(), moovPos, SEEK_SET);
  if (fread(m.data(), 1, m.size(), f.get()) != m.size()) return false;
  // walk boxes inside [a, b); call fn(type, start, end) for each
  std::function<void(size_t, size_t, const std::function<void(const char *, size_t, size_t)> &)> each =
      [&](size_t a, size_t b, const std::function<void(const char *, size_t, size_t)> &fn) {
        while (a + 8 <= b) {
          uint64_t sz = be32(&m[a]);
          size_t hdr = 8;
          if (sz == 1 && a + 16 <= b) {
            sz = be64(&m[a + 8]);
            hdr = 16;
          }
          if (sz < hdr || a + sz > b) break;
          fn((const char *)&m[a + 4], a + hdr, a + (size_t)sz);
          a += (size_t)sz;
        }
      };
  bool found = false;
  each(0, m.size(), [&](const char *t, size_t a, size_t b) {
    if (found || memcmp(t, "trak", 4)) return;
    bool isTc = false;
    uint32_t timescale = 0, frameDur = 0, chunkOff = 0;
    uint64_t chunkOff64 = 0;
    int nFrames = 0;
    std::function<void(const char *, size_t, size_t)> walk = [&](const char *tt, size_t aa, size_t bb) {
      if (!memcmp(tt, "mdia", 4) || !memcmp(tt, "minf", 4) || !memcmp(tt, "stbl", 4)) each(aa, bb, walk);
      else if (!memcmp(tt, "hdlr", 4) && aa + 12 <= bb) isTc = isTc || !memcmp(&m[aa + 8], "tmcd", 4);
      else if (!memcmp(tt, "stsd", 4) && aa + 8 + 16 + 17 <= bb) {
        size_t e = aa + 8; // first sample description
        if (!memcmp(&m[e + 4], "tmcd", 4)) {
          size_t d = e + 8 + 8; // after size, format, 6 reserved + 2 data reference index
          timescale = be32(&m[d + 8]);
          frameDur = be32(&m[d + 12]);
          nFrames = m[d + 16];
        }
      } else if (!memcmp(tt, "stco", 4) && aa + 12 <= bb && be32(&m[aa + 4]) > 0)
        chunkOff = be32(&m[aa + 8]);
      else if (!memcmp(tt, "co64", 4) && aa + 16 <= bb && be32(&m[aa + 4]) > 0)
        chunkOff64 = be64(&m[aa + 8]);
    };
    each(a, b, walk);
    if (!isTc || !timescale || !frameDur) return;
    uint64_t off = chunkOff64 ? chunkOff64 : chunkOff;
    unsigned char fr[4];
    fseek64(f.get(), (int64_t)off, SEEK_SET);
    if (fread(fr, 1, 4, f.get()) != 4) return;
    double frame = be32(fr);
    *seconds = frame * frameDur / timescale;
    (void)nFrames;
    found = true;
  });
  return found;
}

bool videoStartTimecode(const std::string &path, double *seconds) {
  std::string e = path.size() > 4 ? path.substr(path.size() - 4) : path;
  for (auto &c : e) c = (char)tolower((unsigned char)c);
  if (e == ".mxf") return mxfStartTimecode(path, seconds);
  if (e == ".mov" || e == ".mp4" || e == ".m4v") return movStartTimecode(path, seconds);
  return false;
}

// ---------------------------------------------------------------- BWF writer

namespace {
class BwfWriter {
public:
  bool open(const std::string &path, int ch, int rate, int bits, bool isFloat, int64_t timeRef,
            const std::string &desc) {
    f_.reset(openUtf8(path, "wb"));
    if (!f_) return false;
    ch_ = ch;
    ba_ = ch * bits / 8;
    put("RIFF", 4); u32(0); put("WAVE", 4);
    // JUNK placeholder, turned into ds64 if the file grows past 4 GB (RF64)
    put("JUNK", 4); u32(28); zero(28);
    // bext
    put("bext", 4); u32(602);
    char d[256] = {0};
    strncpy(d, desc.c_str(), 255);
    put(d, 256);
    char orig[32] = {0};
    strncpy(orig, "OLAAF", 31);
    put(orig, 32);
    zero(32); // originator reference
    char date[11], tm[9];
    time_t t = time(nullptr);
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    strftime(date, sizeof(date), "%Y-%m-%d", &lt);
    strftime(tm, sizeof(tm), "%H:%M:%S", &lt);
    put(date, 10); put(tm, 8);
    u64(timeRef > 0 ? (uint64_t)timeRef : 0);
    u16(1);    // version
    zero(64);  // UMID
    zero(10);  // loudness
    zero(180); // reserved
    // fmt
    bool ext = ch > 2;
    put("fmt ", 4); u32(ext ? 40 : 16);
    u16(ext ? 0xFFFE : (isFloat ? 3 : 1));
    u16((uint16_t)ch); u32((uint32_t)rate); u32((uint32_t)(rate * ba_)); u16((uint16_t)ba_); u16((uint16_t)bits);
    if (ext) {
      u16(22); u16((uint16_t)bits); u32(0);
      static const unsigned char pcm[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
      static const unsigned char flt[16] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
      put(isFloat ? flt : pcm, 16);
    }
    put("data", 4);
    dataSizePos_ = ftell64(f_.get());
    u32(0);
    return !ferror(f_.get());
  }
  bool write(const void *p, size_t n) {
    bytes_ += n;
    return fwrite(p, 1, n, f_.get()) == n;
  }
  bool close() {
    if (!f_) return false;
    if (bytes_ & 1) fputc(0, f_.get());
    int64_t end = ftell64(f_.get());
    bool big = (uint64_t)end > 0xFFFFFFFFull;
    fseek64(f_.get(), 0, SEEK_SET);
    if (big) {
      put("RF64", 4); u32(0xFFFFFFFF);
      fseek64(f_.get(), 12, SEEK_SET);
      put("ds64", 4); u32(28);
      u64((uint64_t)end - 8); u64(bytes_); u64(bytes_ / (uint64_t)ba_); u32(0);
      fseek64(f_.get(), dataSizePos_, SEEK_SET);
      u32(0xFFFFFFFF);
    } else {
      fseek64(f_.get(), 4, SEEK_SET);
      u32((uint32_t)(end - 8));
      fseek64(f_.get(), dataSizePos_, SEEK_SET);
      u32((uint32_t)bytes_);
    }
    bool ok = !ferror(f_.get());
    f_.reset();
    return ok;
  }

private:
  void put(const void *p, size_t n) { fwrite(p, 1, n, f_.get()); }
  void zero(size_t n) { static const char z[256] = {0}; while (n) { size_t m = n < 256 ? n : 256; put(z, m); n -= m; } }
  void u16(uint16_t v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; put(b, 2); }
  void u32(uint32_t v) { unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; put(b, 4); }
  void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
  FilePtr f_;
  int ch_ = 1, ba_ = 3;
  uint64_t bytes_ = 0;
  int64_t dataSizePos_ = 0;
};
} // namespace

static void swapBytes(unsigned char *p, size_t n, int bytesPerSample) {
  for (size_t i = 0; i + (size_t)bytesPerSample <= n; i += (size_t)bytesPerSample)
    for (int a = 0, b = bytesPerSample - 1; a < b; a++, b--) std::swap(p[i + a], p[i + b]);
}

// ---------------------------------------------------------------- reading sample frames

bool PcmFile::open(const std::string &path, const PcmInfo &info) {
  close();
  f_ = openUtf8(path, "rb");
  if (!f_) return false;
  in_ = info;
  seg_ = 0;
  return true;
}

void PcmFile::close() {
  if (f_) fclose(f_);
  f_ = nullptr;
}

int64_t PcmFile::read(int64_t frame, int64_t n, unsigned char *out) {
  if (!f_ || n <= 0 || frame < 0) return 0;
  const int fb = frameBytes();
  if (fb <= 0) return 0;
  if (in_.segs.empty()) { // one block
    int64_t total = in_.frames();
    if (frame >= total) return 0;
    n = std::min(n, total - frame);
    if (fseek64(f_, (int64_t)in_.dataOffset + frame * fb, SEEK_SET)) return 0;
    return (int64_t)(fread(out, (size_t)fb, (size_t)n, f_));
  }
  // packets: find the one holding `frame` (usually the same or the next one)
  const auto &sg = in_.segs;
  if (seg_ >= sg.size() || sg[seg_].firstFrame > frame) {
    size_t lo = 0, hi = sg.size();
    while (hi - lo > 1) {
      size_t mid = (lo + hi) / 2;
      if (sg[mid].firstFrame <= frame) lo = mid;
      else hi = mid;
    }
    seg_ = lo;
  }
  int64_t got = 0;
  while (got < n && seg_ < sg.size()) {
    const PcmSeg &p = sg[seg_];
    int64_t in = frame + got - p.firstFrame;
    if (in >= p.frames) {
      seg_++;
      continue;
    }
    int64_t take = std::min(n - got, p.frames - in);
    if (!in_.aes3) {
      if (fseek64(f_, (int64_t)p.offset + in * fb, SEEK_SET)) break;
      size_t r = fread(out + got * fb, (size_t)fb, (size_t)take, f_);
      got += (int64_t)r;
      if ((int64_t)r < take) break;
    } else {
      const int slot = in_.aesChannels * 4;
      tmp_.resize((size_t)(take * slot));
      if (fseek64(f_, (int64_t)p.offset + 4 + in * slot, SEEK_SET)) break;
      size_t r = fread(tmp_.data(), (size_t)slot, (size_t)take, f_);
      for (size_t i = 0; i < r; i++)
        for (int c = 0; c < in_.channels; c++) {
          const unsigned char *w = &tmp_[i * (size_t)slot + (size_t)c * 4];
          uint32_t word = (uint32_t)w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24;
          uint32_t v = (word >> 4) & 0xffffff;
          unsigned char *d = out + (got + (int64_t)i) * fb + c * 3;
          d[0] = (unsigned char)v;
          d[1] = (unsigned char)(v >> 8);
          d[2] = (unsigned char)(v >> 16);
        }
      got += (int64_t)r;
      if ((int64_t)r < take) break;
    }
  }
  return got;
}

bool convertToBwf(const std::string &src, const PcmInfo &in, const std::string &dst, int64_t timeRef,
                  const std::string &desc, std::string *error) {
  PcmFile pf;
  if (!pf.open(src, in)) {
    if (error) *error = "cannot read " + src;
    return false;
  }
  BwfWriter w;
  if (!w.open(dst, in.channels, in.rate, in.bits, in.isFloat, timeRef, desc)) {
    if (error) *error = "cannot write " + dst;
    return false;
  }
  int bps = in.bits / 8, fb = pf.frameBytes();
  const int64_t block = 65536;
  std::vector<unsigned char> buf((size_t)(block * fb));
  int64_t total = in.frames();
  for (int64_t at = 0; at < total;) {
    int64_t r = pf.read(at, std::min(block, total - at), buf.data());
    if (r <= 0) break;
    if (in.bigEndian) swapBytes(buf.data(), (size_t)(r * fb), bps);
    if (!w.write(buf.data(), (size_t)(r * fb))) {
      if (error) *error = "disk full while writing " + dst;
      return false;
    }
    at += r;
  }
  return w.close();
}

bool interleaveToBwf(const std::vector<std::string> &srcs, const std::vector<PcmInfo> &ins, const std::string &dst,
                     int64_t timeRef, const std::string &desc, std::string *error) {
  size_t nch = srcs.size();
  if (!nch || ins.size() != nch) return false;
  int bits = ins[0].bits, rate = ins[0].rate;
  for (auto &i : ins)
    if (i.bits != bits || i.rate != rate || i.channels != 1 || i.isFloat != ins[0].isFloat) {
      if (error) *error = "sources differ in format";
      return false;
    }
  int bps = bits / 8;
  std::vector<std::unique_ptr<PcmFile>> fs;
  int64_t frames = 0;
  for (size_t c = 0; c < nch; c++) {
    fs.emplace_back(new PcmFile);
    if (!fs.back()->open(srcs[c], ins[c])) {
      if (error) *error = "cannot read " + srcs[c];
      return false;
    }
    frames = std::max(frames, ins[c].frames());
  }
  BwfWriter w;
  if (!w.open(dst, (int)nch, rate, bits, ins[0].isFloat, timeRef, desc)) {
    if (error) *error = "cannot write " + dst;
    return false;
  }
  const int64_t block = 65536;
  std::vector<std::vector<unsigned char>> in(nch, std::vector<unsigned char>((size_t)(block * bps)));
  std::vector<unsigned char> out((size_t)(block * bps) * nch);
  std::vector<int64_t> left(nch);
  for (size_t c = 0; c < nch; c++) left[c] = ins[c].frames();
  for (int64_t done = 0; done < frames; done += block) {
    int64_t n = std::min(block, frames - done);
    for (size_t c = 0; c < nch; c++) {
      int64_t want = std::max<int64_t>(0, std::min(n, left[c]));
      size_t got = want ? (size_t)fs[c]->read(done, want, in[c].data()) : 0;
      if (ins[c].bigEndian) swapBytes(in[c].data(), got * (size_t)bps, bps);
      memset(in[c].data() + got * (size_t)bps, 0, (size_t)(n - (int64_t)got) * (size_t)bps);
      left[c] -= (int64_t)got;
    }
    for (int64_t i = 0; i < n; i++)
      for (size_t c = 0; c < nch; c++)
        memcpy(&out[((size_t)i * nch + c) * (size_t)bps], &in[c][(size_t)i * (size_t)bps], (size_t)bps);
    if (!w.write(out.data(), (size_t)n * nch * (size_t)bps)) {
      if (error) *error = "disk full while writing " + dst;
      return false;
    }
  }
  return w.close();
}

} // namespace olaaf
