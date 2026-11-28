// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Is there a newer OLAAF? Asks GitHub once per REAPER session, in the background, and gives up
// quietly when there is no connection or no answer within a few seconds.

#pragma once
#include <string>

namespace olaaf {
#define OLAAF_VERSION_STRING "0.8.7"

void updateCheckStart();
// 0 still asking, 1 a newer release (tag and page filled in), 2 nothing newer or no answer
int updateCheckState(std::string *tag, std::string *url);
// The newer release's installer for this system, straight from GitHub: OLAAF-x.y.z-mac.dmg on macOS,
// OLAAF-x.y.z-win64.exe on Windows. "" when there is none (Linux): then the release page is the way.
std::string updateAssetUrl();
// where downloads go: the folder chosen once (kept by the host), else Downloads in the home folder
std::string updateFolder();
// save: also keep it for the next REAPER session (through the saver the extension sets once)
void setUpdateFolder(const std::string &dir, bool save = false);
void setUpdateFolderSaver(void (*save)(const std::string &dir));
// downloads the installer into updateFolder() in the background; install: opens it when it is there
// (macOS: the installer from the disk image, Windows: the setup)
void updateDownloadStart(bool install);
// 0 not started, 1 downloading (progress 0..1, -1 = size unknown), 2 there (path), 3 failed (error)
int updateDownloadState(double *progress, std::string *path, std::string *error);
// opens a downloaded installer (as above) / the disk image
void openInstaller(const std::string &path);
// opens a GitHub page in the browser (only github.com addresses)
void openWebPage(const std::string &url);
// shows the file in the Finder (macOS) / Explorer (Windows) / the file manager (Linux), selected
void revealInFileBrowser(const std::string &path);
} // namespace olaaf
