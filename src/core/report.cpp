// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "report.h"

#include <cmath>
#include <cstdio>
#include <ctime>

#include "audiofile.h"
#include "util.h"

namespace olaaf {

static std::string fpsName(double f) {
  if (std::fabs(f - std::lround(f)) < 0.001) return strprintf("%d fps", (int)std::lround(f));
  return strprintf("%.3f fps", f);
}

// A readable report of the import, next to the REAPER project (or in the audio folder while the
// project is not saved yet). The technical log is added at the end. Returns its path.
std::string writeImportReport(Session &s, const ImportOptions &opt, const MediaReport &rep, int merged, double pf,
                              double ps, const std::string &rppPath, const char *version) {
  const char *rpp = rppPath.c_str();
  std::string dir = rpp[0] ? dirName(rpp) : (!rep.logPath.empty() ? dirName(rep.logPath) : opt.destDir);
  if (dir.empty()) return std::string();
  std::string path = joinPath(dir, "OLAAF import report - " + safeFileName(baseName(s.aafPath)) + ".txt");
  std::string r;
  time_t now = time(nullptr);
  char when[64] = "";
  strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
  r += "OLAAF import report\n===================\n\n";
  r += strprintf("Date:            %s   (OLAAF %s)\n", when, version);
  r += "AAF:             " + baseName(s.aafPath) + "\n                 " + s.aafPath + "\n";
  r += strprintf("Made with:       %s %s %s\n", s.company.c_str(), s.product.c_str(), s.version.c_str());
  r += std::string("REAPER project:  ") + (rpp[0] ? rpp : "(not saved yet)") + "\n\n";

  // the checks
  bool fpsOk = pf <= 0 || std::fabs(pf - s.fps) < 0.01, srOk = ps <= 0 || std::fabs(ps - s.sampleRate) < 1;
  int missing = rep.missing;
  r += "Checks\n------\n";
  r += strprintf("Frame rate:      AAF %s, project %s   %s\n", fpsName(s.fps).c_str(), pf > 0 ? fpsName(pf).c_str() : "unknown",
                 pf <= 0 ? "" : fpsOk ? "OK" : "MISMATCH");
  r += strprintf("Sample rate:     AAF %d Hz, project %s   %s\n", s.sampleRate, ps > 0 ? strprintf("%g Hz", ps).c_str() : "unknown",
                 ps <= 0 ? "" : srOk ? "OK" : "MISMATCH");
  r += strprintf("Media:           %d found, %d missing   %s\n\n", rep.found, missing, missing ? "CHECK" : "OK");

  r += "Placement\n---------\n";
  r += "AAF starts at:   " + formatTC(s.firstClip(), s.fps) + "\n";
  r += "In REAPER at:    " + formatTC(s.firstClip() + opt.tcOffset, s.fps) + "\n";
  r += std::string("Offset:          ") + (opt.tcOffset < 0 ? "-" : "+") + formatTC(std::fabs(opt.tcOffset), s.fps) + "\n\n";

  r += "Options\n-------\n";
  r += std::string("Fades:           ") + (opt.fades ? "on" : "off") + "\n";
  r += strprintf("Stereo:          %s%s\n", opt.mergeStereo ? "combine .L / .R" : "as in the AAF",
                 merged ? strprintf(" (%d pairs combined)", merged).c_str() : "");
  r += std::string("Clip gain:       ") + (!opt.clipGain ? "off" : opt.clipGainToTrack ? "on the track" : "on the items") + "\n";
  r += std::string("Markers:         ") + (!opt.markers ? std::string("off") : s.markers.empty() ? std::string("none in the AAF") : strprintf("%zu", s.markers.size())) + "\n";
  r += std::string("Folders:         ") + (opt.folders ? "on" : "off") + "\n";
  r += std::string("Video:           ") + (s.video.empty() ? "none in the AAF" : !opt.video ? std::string("off")
                                       : s.video[0].foundPath.empty() ? "not found: " + s.video[0].name
                                                                      : baseName(s.video[0].foundPath)) + "\n";
  r += std::string("Audio:           ") + (opt.copyToSubdir ? "copied into " : "left in place; converted files in ") +
       (rep.logPath.empty() ? opt.destDir : dirName(rep.logPath)) + "\n";
  r += strprintf("Files written:   %d converted to Broadcast WAV, %d copied, %d extracted, %d combined\n\n", rep.converted,
                 rep.copied, rep.extracted, rep.interleaved);

  int nt = 0;
  for (auto &t : s.tracks) nt += t.import;
  r += strprintf("Tracks (%d imported)\n--------------------\n", nt);
  for (auto &t : s.tracks) {
    if (!t.import) continue;
    int ev = 0, mu = 0, rm = 0, sw = 0;
    for (auto &c : t.clips) {
      if (c.excluded) rm++;
      else ev++;
      mu += c.mute && !c.excluded;
      sw += c.swapLR && !c.excluded;
    }
    std::string line = "  " + t.name + strprintf("  (%s)", t.channels == 1 ? "mono" : t.channels == 2 ? "stereo" : "multichannel");
    if (opt.folders && !t.group.empty())
      if (Folder *f = s.folder(t.group)) line += "  folder: " + f->name;
    if (!t.targetName.empty()) line += "  onto REAPER track: " + t.targetName;
    line += strprintf("  - %d event%s", ev, ev == 1 ? "" : "s");
    if (mu) line += strprintf(", %d muted", mu);
    if (rm) line += strprintf(", %d removed", rm);
    if (sw) line += strprintf(", %d with left / right exchanged", sw);
    r += line + "\n";
  }
  bool anyOff = false;
  for (auto &t : s.tracks)
    if (!t.import) {
      if (!anyOff) r += "\nNot imported (unticked):\n";
      anyOff = true;
      r += "  " + t.name + "\n";
    }
  if (!rep.missingNames.empty()) {
    r += "\nMissing media (offline items in REAPER)\n---------------------------------------\n";
    for (auto &n : rep.missingNames) r += "  " + n + "\n";
  }
  int shown = 0;
  for (auto &w : s.warnings) {
    if (w.rfind("AAF: ", 0) == 0) continue;
    if (!shown++) r += "\nNotes\n-----\n";
    r += "  " + w + "\n";
  }
  // the technical log
  if (!rep.logPath.empty())
    if (FILE *lf = openUtf8(rep.logPath, "rb")) {
      r += "\n\nTechnical details\n=================\n\n";
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), lf)) > 0) r.append(buf, n);
      fclose(lf);
      std::remove(rep.logPath.c_str());
    }
  FILE *f = openUtf8(path, "wb");
  if (!f) return std::string();
  fwrite(r.data(), 1, r.size(), f);
  fclose(f);
  return path;
}


} // namespace olaaf
