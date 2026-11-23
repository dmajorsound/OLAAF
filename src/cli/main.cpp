// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// olaaf-cli: converts an AAF into a REAPER project (.rpp) using the same code as the extension.
// Used for testing without REAPER and for batch conversion.

#include "../core/importer.h"
#include "../core/util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace olaaf;

static void usage() {
  printf("olaaf-cli - AAF to REAPER project\n\n"
         "usage: olaaf-cli [options] input.aaf output.rpp\n\n"
         "  --dest <dir>        where audio is written (default: folder of output.rpp)\n"
         "  --subdir <name>     subfolder name (default: <AAF name>_Audio)\n"
         "  --copy              copy WAVs into the subfolder (default: reference them where they are)\n"
         "  --media <dir>       extra folder to search for linked media (repeatable)\n"
         "  --offset <TC>       timecode offset, e.g. -10:00:00:00\n"
         "  --no-merge          keep .L/.R tracks separate\n"
         "  --no-fades          skip fades\n"
         "  --no-gain           skip clip gain\n"
         "  --no-pt-fades       keep Pro Tools rendered fade clips\n"
         "  --relative          media paths relative to the .rpp (portable project folder)\n"
         "  --list              only print what would be imported\n");
}

int main(int argc, char **argv) {
  ImportOptions opt;
  std::string in, out, offset;
  bool list = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--dest") opt.destDir = next();
    else if (a == "--subdir") opt.subdir = next();
    else if (a == "--no-copy") opt.copyToSubdir = false;
    else if (a == "--copy") opt.copyToSubdir = true;
    else if (a == "--media") opt.searchFolders.push_back(next());
    else if (a == "--offset") offset = next();
    else if (a == "--no-merge") opt.mergeStereo = false;
    else if (a == "--no-fades") opt.fades = false;
    else if (a == "--no-gain") opt.clipGain = false;
    else if (a == "--gain-to-track") opt.clipGainToTrack = true;
    else if (a == "--no-pt-fades") opt.read.proToolsTrueFades = false;
    else if (a == "--list") list = true;
    else if (a == "--relative") opt.relativeTo = "*";
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (in.empty()) in = a;
    else out = a;
  }
  if (in.empty() || (out.empty() && !list)) { usage(); return 1; }

  Importer imp;
  std::string err;
  if (!imp.load(in, opt, &err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
  Session &s = imp.session();
  if (!offset.empty() && !parseTC(offset, s.fps, &opt.tcOffset)) {
    fprintf(stderr, "error: bad timecode offset \"%s\"\n", offset.c_str());
    return 1;
  }
  if (opt.destDir.empty()) opt.destDir = out.empty() ? dirName(in) : dirName(out);
  if (opt.relativeTo == "*") opt.relativeTo = dirName(out);

  imp.locateMedia(opt);
  int merged = opt.mergeStereo ? imp.mergeStereoTracks() : 0;

  printf("%s  (%s %s)\n", s.name.c_str(), s.product.c_str(), s.version.c_str());
  printf("%d Hz / %d bit, %.3g fps%s, start %s, first clip %s\n", s.sampleRate, s.bits, s.fps,
         s.dropFrame ? " DF" : "", formatTC(s.start, s.fps).c_str(), formatTC(s.firstClip(), s.fps).c_str());
  int nclips = 0, nfades = 0, ngain = 0;
  for (auto &t : s.tracks) {
    int f = 0, g = 0;
    for (auto &c : t.clips) {
      f += (c.fadeIn.len > 0) + (c.fadeOut.len > 0);
      g += std::fabs(c.gain - 1.0) > 1e-6;
    }
    nclips += (int)t.clips.size();
    nfades += f;
    ngain += g;
    printf("  %-28s %s %3zu clips  %3d fades  %3d gains%s%s\n", t.name.c_str(), t.channels == 2 ? "stereo" : t.channels == 1 ? "mono  " : "multi ",
           t.clips.size(), f, g, t.mergedFrom.empty() ? "" : "   merged from ", t.mergedFrom.c_str());
  }
  int found = 0, missing = 0, embedded = 0;
  for (auto &m : s.media) {
    if (m.kind == MediaKind::Embedded) embedded++;
    else if (fileExists(m.foundPath)) found++;
    else missing++;
  }
  printf("%zu tracks, %d clips, %d fades, %d clip gains, %d stereo merges\n", s.tracks.size(), nclips, nfades, ngain, merged);
  printf("media: %zu files, %d embedded, %d found, %d missing\n", s.media.size(), embedded, found, missing);
  if (list) {
    for (auto &w : s.warnings) printf("warning: %s\n", w.c_str());
    return 0;
  }

  MediaReport rep;
  imp.prepareMedia(opt, &rep, nullptr);
  printf("written: %d extracted, %d converted to BWF, %d copied, %d combined; %d missing\n", rep.extracted,
         rep.converted, rep.copied, rep.interleaved, rep.missing);
  for (auto &w : s.warnings) printf("warning: %s\n", w.c_str());

  FILE *f = fopen(out.c_str(), "wb");
  if (!f) { fprintf(stderr, "error: cannot write %s\n", out.c_str()); return 3; }
  std::string rpp = imp.projectRpp(opt);
  fwrite(rpp.data(), 1, rpp.size(), f);
  fclose(f);
  printf("project: %s\n", out.c_str());
  return 0;
}
