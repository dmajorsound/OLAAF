// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// "OLAAF import report - <AAF>.txt": what was imported, how, and what to check, written for people
// (frame rate and sample rate against the project, placement, options, tracks, missing media),
// followed by the technical log.

#pragma once
#include <string>

#include "importer.h"

namespace olaaf {
// pf / ps: the project's frame rate and sample rate (0 = unknown); rppPath empty = not saved yet.
// The report goes next to the project, else next to the audio. Returns its path ("" on failure).
std::string writeImportReport(Session &s, const ImportOptions &opt, const MediaReport &rep, int merged, double pf, double ps,
                              const std::string &rppPath, const char *version);
} // namespace olaaf
