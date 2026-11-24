// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Several AAFs (reels) in one import: each at its own timecode (plus the offset chosen for it),
// tracks with the same name share one REAPER track.

#pragma once
#include "model.h"

#include <string>
#include <vector>

namespace olaaf {

// short names for the reel tabs: the AAF names, or "Reel 1", "Reel 2" when they don't fit
std::string reelLabel(const Session &s, int index);

// the part that tells the reels apart: "Film_Rolle_1_Mix_v12", "Film_Rolle_2_Mix_v12" -> "Rolle_1", "Rolle_2"
std::vector<std::string> reelShortLabels(const std::vector<std::string> &names);

// Do two reels land on top of each other? (start / end in REAPER time)
bool reelsOverlap(double a0, double a1, double b0, double b1);

// One session from all reels, after their media is ready (clip files set). `shift[i]` is added to
// every position of reel i (its offset minus the first reel's). Reel names go into the warnings.
Session combineReels(const std::vector<Session> &reels, const std::vector<double> &shift);

} // namespace olaaf
