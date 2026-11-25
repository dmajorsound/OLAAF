// olAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace olaaf {

std::string lower(std::string s);
std::string fileExt(const std::string &path);          // "wav" (no dot)
std::string baseName(const std::string &path);         // "file.wav"
std::string stemName(const std::string &path);         // "file"
std::string dirName(const std::string &path);
std::string joinPath(const std::string &a, const std::string &b);
bool fileExists(const std::string &path);
bool makeDirs(const std::string &path);
bool renamePath(const std::string &from, const std::string &to); // replaces `to`
void removePath(const std::string &path);
// deletes the oldest files of a folder (not below it) until it holds at most maxBytes
void trimFolder(const std::string &dir, uint64_t maxBytes);
// the same for a folder of folders: whole subfolders go, the oldest first
void trimSubfolders(const std::string &dir, uint64_t maxBytes);
// size and last change of a file (for caches); false if it isn't there
bool fileStamp(const std::string &path, uint64_t *size, int64_t *changed);
bool copyFile(const std::string &from, const std::string &to);
std::string safeFileName(const std::string &name);     // strips / \ : * ? " < > |
std::string uniquePath(const std::string &path);       // adds -1, -2 … if taken

// "file:///Volumes/x/a%20b.wav" -> "/Volumes/x/a b.wav"
std::string uriToPath(const std::string &uri);

// all files below `dir` (depth-limited), lowercase basename -> full path
void indexFolder(const std::string &dir, int maxDepth,
                 std::vector<std::pair<std::string, std::string>> *out, size_t maxFiles = 400000);

std::string strprintf(const char *fmt, ...);

} // namespace olaaf
