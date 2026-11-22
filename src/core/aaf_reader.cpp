// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Reads an AAF with libAAF and fills the olAAF session model.

#include "aaf_reader.h"
#include "util.h"

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <map>

extern "C" {
#include <libaaf.h>
#include <libaaf/ProTools.h>
}

namespace olaaf {

static double rat(const aafRational_t *r) {
  return (r && r->denominator) ? (double)r->numerator / (double)r->denominator : 0.0;
}

// edit units -> seconds
static double eu2s(aafPosition_t v, const aafRational_t *editRate) {
  double er = rat(editRate);
  return er > 0 ? (double)v / er : 0.0;
}

static int fadeShape(uint32_t flags) {
  // libAAF interpolation -> nearest REAPER fade shape (linear is exact, the rest approximate)
  if (flags & AAFI_INTERPOL_LINEAR) return 0;
  if (flags & AAFI_INTERPOL_POWER) return 1;
  if (flags & AAFI_INTERPOL_LOG) return 1;
  if (flags & AAFI_INTERPOL_BSPLINE) return 5;
  return 0;
}

static MediaKind kindFromUri(const std::string &uri) {
  std::string e = lower(fileExt(uri));
  if (e == "wav" || e == "bwf" || e == "wave") return MediaKind::Wav;
  if (e == "aif" || e == "aiff" || e == "aifc") return MediaKind::Aiff;
  if (e == "mxf") return MediaKind::Mxf;
  return MediaKind::Unknown;
}

// collect libAAF messages as warnings instead of printing them
struct LogSink {
  std::vector<std::string> *out;
};

static void logCallback(struct aafLog *log, void *ctxdata, int lib, int type, const char *srcfile,
                        const char *srcfunc, int lineno, const char *msg, void *user) {
  (void)log; (void)ctxdata; (void)lib; (void)srcfile; (void)srcfunc; (void)lineno;
  LogSink *s = (LogSink *)user;
  if (!s || !s->out || !msg) return;
  if (type == VERB_ERROR) s->out->push_back(std::string("AAF: ") + msg);
}

AafFile::AafFile() {}

AafFile::~AafFile() {
  if (aafi_) aafi_release(&aafi_);
}

bool AafFile::load(const std::string &path, const ReadOptions &opt, Session *s, std::string *error) {
  static LogSink sink;
  sink.out = &s->warnings;

  aafi_ = aafi_alloc(nullptr);
  if (!aafi_) {
    if (error) *error = "Could not start the AAF reader.";
    return false;
  }
  aafi_set_debug(aafi_, VERB_ERROR, 0, nullptr, logCallback, &sink);
  int pt = 0;
  if (opt.proToolsTrueFades) pt |= AAFI_PROTOOLS_OPT_REPLACE_CLIP_FADES;
  if (opt.proToolsRemoveSAE) pt |= AAFI_PROTOOLS_OPT_REMOVE_SAMPLE_ACCURATE_EDIT;
  aafi_set_option_int(aafi_, "protools", pt);
  // let libAAF look for linked media next to the AAF; olAAF does its own search afterwards
  std::string dir = dirName(path);
  aafi_set_option_str(aafi_, "media_location", dir.c_str());

  if (aafi_load_file(aafi_, path.c_str())) {
    if (error) *error = "This file could not be read as AAF.";
    return false;
  }

  AAF_Iface *a = aafi_;
  s->aafPath = path;
  s->name = a->compositionName ? a->compositionName : baseName(path);
  if (a->aafd && a->aafd->Identification.CompanyName) s->company = a->aafd->Identification.CompanyName;
  if (a->aafd && a->aafd->Identification.ProductName) s->product = a->aafd->Identification.ProductName;
  if (a->aafd && a->aafd->Identification.ProductVersionString)
    s->version = a->aafd->Identification.ProductVersionString;
  if (a->Timecode) {
    s->fps = a->Timecode->fps ? a->Timecode->fps : 25;
    s->dropFrame = a->Timecode->drop != 0;
  }
  if (a->Audio) {
    s->sampleRate = a->Audio->samplerate ? (int)a->Audio->samplerate : 48000;
    s->bits = a->Audio->samplesize ? a->Audio->samplesize : 24;
  }
  s->start = eu2s(a->compositionStart, a->compositionStart_editRate);
  s->end = s->start + eu2s(a->compositionLength, a->compositionLength_editRate);

  // media
  std::map<aafiAudioEssenceFile *, int> mediaIndex;
  aafiAudioEssenceFile *ef = nullptr;
  AAFI_foreachAudioEssenceFile(a, ef) {
    Media m;
    m.name = ef->name ? ef->name : "";
    m.uniqueName = ef->unique_name ? ef->unique_name : m.name;
    m.originalUri = ef->original_file_path ? ef->original_file_path : "";
    if (ef->usable_file_path) m.foundPath = ef->usable_file_path;
    m.kind = ef->is_embedded ? MediaKind::Embedded : kindFromUri(m.originalUri);
    m.channels = ef->channels ? ef->channels : 1;
    m.sampleRate = ef->samplerate ? (int)ef->samplerate : s->sampleRate;
    m.bits = ef->samplesize ? ef->samplesize : s->bits;
    m.length = ef->length;
    // source mob origin -> BWF time reference (samples since midnight)
    m.timeReference = ef->timeReference;
    m.aafEssence = ef;
    mediaIndex[ef] = (int)s->media.size();
    s->media.push_back(m);
  }

  // tracks and clips
  aafiAudioTrack *tr = nullptr;
  AAFI_foreachAudioTrack(a, tr) {
    Track t;
    t.name = tr->name ? tr->name : ("Audio " + std::to_string(tr->number));
    t.channels = (tr->format >= 1 && tr->format <= 8) ? tr->format : 0;
    if (tr->gain && (tr->gain->flags & AAFI_AUDIO_GAIN_CONSTANT) && tr->gain->value)
      t.volume = rat(&tr->gain->value[0]);

    aafPosition_t startEU = aafi_convertUnit(a->compositionStart, a->compositionStart_editRate, tr->edit_rate);
    // track automation: times are normalized (0..1) over the track, from the composition start
    double trStart = eu2s(startEU, tr->edit_rate), trLen = eu2s(tr->current_pos, tr->edit_rate);
    if (tr->pan) {
      if ((tr->pan->flags & AAFI_AUDIO_GAIN_CONSTANT) && tr->pan->value)
        t.pan = rat(&tr->pan->value[0]) * 2.0 - 1.0; // AAF 0 = left, 0.5 = center, 1 = right
      else if (tr->pan->flags & AAFI_AUDIO_GAIN_VARIABLE)
        for (unsigned i = 0; i < tr->pan->pts_cnt; i++)
          t.panEnv.push_back({trStart + rat(&tr->pan->time[i]) * trLen, rat(&tr->pan->value[i]) * 2.0 - 1.0});
    }
    if (tr->gain && (tr->gain->flags & AAFI_AUDIO_GAIN_VARIABLE))
      for (unsigned i = 0; i < tr->gain->pts_cnt; i++)
        t.volEnv.push_back({trStart + rat(&tr->gain->time[i]) * trLen, rat(&tr->gain->value[i])});
    aafiTimelineItem *it = nullptr;
    int prev = -1;
    aafiTransition *pendingXfade = nullptr;
    int maxCh = 0;

    AAFI_foreachTrackItem(tr, it) {
      aafiTransition *x = aafi_timelineItemToCrossFade(it);
      if (x) {
        if (x->flags & AAFI_TRANS_XFADE) {
          pendingXfade = x;
          if (prev >= 0) {
            t.clips[prev].fadeOut.len = eu2s(x->len, tr->edit_rate);
            t.clips[prev].fadeOut.shape = fadeShape(x->flags);
          }
        }
        continue;
      }
      aafiAudioClip *c = aafi_timelineItemToAudioClip(it);
      if (!c) continue;

      Clip k;
      k.pos = eu2s(c->pos + startEU, tr->edit_rate);
      k.len = eu2s(c->len, tr->edit_rate);
      k.srcOffset = eu2s(c->essence_offset, tr->edit_rate);
      k.mute = c->mute != 0;
      if (c->gain && (c->gain->flags & AAFI_AUDIO_GAIN_CONSTANT) && c->gain->value)
        k.gain = rat(&c->gain->value[0]);
      if (c->automation && (c->automation->flags & AAFI_AUDIO_GAIN_VARIABLE)) {
        for (unsigned i = 0; i < c->automation->pts_cnt; i++)
          k.gainEnv.push_back({rat(&c->automation->time[i]) * k.len, rat(&c->automation->value[i])});
      }
      if (pendingXfade) {
        k.fadeIn.len = eu2s(pendingXfade->len, tr->edit_rate);
        k.fadeIn.shape = fadeShape(pendingXfade->flags);
        pendingXfade = nullptr;
      }
      if (aafiTransition *fi = aafi_getFadeIn(c)) {
        k.fadeIn.len = eu2s(fi->len, tr->edit_rate);
        k.fadeIn.shape = fadeShape(fi->flags);
      }
      if (aafiTransition *fo = aafi_getFadeOut(c)) {
        k.fadeOut.len = eu2s(fo->len, tr->edit_rate);
        k.fadeOut.shape = fadeShape(fo->flags);
      }

      aafiAudioEssencePointer *ep = nullptr;
      AAFI_foreachEssencePointer(c->essencePointerList, ep) {
        Source src;
        auto f = mediaIndex.find(ep->essenceFile);
        src.media = f != mediaIndex.end() ? f->second : -1;
        src.channel = (int)ep->essenceChannel;
        k.sources.push_back(src);
      }
      if (c->subClipName) k.name = c->subClipName;
      else if (!k.sources.empty() && k.sources[0].media >= 0) k.name = s->media[k.sources[0].media].name;

      int ch = c->channels > 0 ? c->channels : (int)k.sources.size();
      if (ch > maxCh) maxCh = ch;
      t.clips.push_back(k);
      prev = (int)t.clips.size() - 1;
    }
    if (t.channels == 0) t.channels = maxCh > 0 ? maxCh : 1;
    if (!t.clips.empty() || t.channels) s->tracks.push_back(t);
  }
  return true;
}

std::string AafFile::extractEmbedded(const Media &m, const std::string &dir) {
  if (!aafi_ || !m.aafEssence) return "";
  char *usable = nullptr;
  aafiAudioEssenceFile *ef = (aafiAudioEssenceFile *)m.aafEssence;
  if (aafi_extractAudioEssenceFile(aafi_, ef, AAFI_EXTRACT_BWAV, dir.c_str(), 0, 0, nullptr, &usable) != 0)
    return "";
  std::string p = usable ? usable : (ef->usable_file_path ? ef->usable_file_path : "");
  free(usable);
  return p;
}

} // namespace olaaf
