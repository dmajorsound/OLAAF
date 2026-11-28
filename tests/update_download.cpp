// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.
//
// Run by the GitHub build on macOS and Windows: downloads a released installer the way the update note in
// the footer does (same code), and checks that it arrived whole.
//   olaaf_update_test <release asset URL> <folder>

#include "../src/ext/update.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace olaaf {
void updateSetAssetForTest(const std::string &url);
}
using namespace olaaf;

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: olaaf_update_test <url> <folder>\n");
    return 2;
  }
  setUpdateFolder(argv[2]);
  updateSetAssetForTest(argv[1]);
  updateDownloadStart(false);
  double prog = 0;
  std::string path, err;
  int st = 1;
  for (int i = 0; i < 1800 && (st = updateDownloadState(&prog, &path, &err)) == 1; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (st != 2) {
    fprintf(stderr, "download failed (state %d): %s\n", st, err.c_str());
    return 1;
  }
  FILE *f = fopen(path.c_str(), "rb");
  long size = -1;
  if (f) {
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
  }
  printf("downloaded %s (%ld bytes)\n", path.c_str(), size);
  return size > 500000 ? 0 : 1; // an installer, not an error page
}
