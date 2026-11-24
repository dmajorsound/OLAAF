// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Is there a newer OLAAF? Asks GitHub once per REAPER session, in the background, and gives up
// quietly when there is no connection or no answer within a few seconds.

#pragma once
#include <string>

namespace olaaf {
#define OLAAF_VERSION_STRING "0.7.3"

void updateCheckStart();
// 0 still asking, 1 a newer release (tag and page filled in), 2 nothing newer or no answer
int updateCheckState(std::string *tag, std::string *url);
// opens a GitHub page in the browser (only github.com addresses)
void openWebPage(const std::string &url);
// shows the file in the Finder (macOS) / Explorer (Windows) / the file manager (Linux), selected
void revealInFileBrowser(const std::string &path);
} // namespace olaaf
