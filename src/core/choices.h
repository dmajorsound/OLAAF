// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// The choices made in the dialog for one AAF (track names, folders, colors, removed events, events
// moved to other tracks, options), so the next import of the same AAF starts where the last one ended.

#pragma once
#include "importer.h"

#include <string>

namespace olaaf {

// file name for the AAF's choices ("3f2a91c0d4e5b677.txt")
std::string choicesFileName(const std::string &aafPath);

std::string choicesToText(const Session &s, const ImportOptions &opt);

// Puts the choices back. Nothing changes (false) when they don't fit this AAF any more (it was
// exported again with other events). `why` says what came back or why not.
bool applyChoices(const std::string &text, Importer &imp, ImportOptions &opt, std::string *why);

} // namespace olaaf
