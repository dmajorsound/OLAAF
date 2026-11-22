// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#pragma once
#include "model.h"

struct AAF_Iface; // libAAF

namespace olaaf {

struct ReadOptions {
  bool proToolsTrueFades = true;   // turn Pro Tools rendered fade clips into real fades
  bool proToolsRemoveSAE = true;   // drop Pro Tools "sample accurate edit" micro clips
};

// Keeps the libAAF context alive: embedded media is extracted from it later.
class AafFile {
public:
  AafFile();
  ~AafFile();
  AafFile(const AafFile &) = delete;
  AafFile &operator=(const AafFile &) = delete;

  // Parses the AAF into `out`. Returns false and sets `error` on failure.
  bool load(const std::string &path, const ReadOptions &opt, Session *out, std::string *error);

  // Writes an embedded essence as a WAV file into `dir`. Returns the written path or "".
  std::string extractEmbedded(const Media &m, const std::string &dir);

private:
  ::AAF_Iface *aafi_ = nullptr;
};

} // namespace olaaf
