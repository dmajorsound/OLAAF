// OLAAF - AAF import for REAPER
// Copyright (C) 2026 dmajorsound. Licensed under the GNU GPL v2 or later.

#include "fonts.h"

#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#endif

namespace olaaf {

#if defined(_WIN32) || defined(__APPLE__)
struct EmbeddedFont { const unsigned char *data; size_t size; };
extern const EmbeddedFont kEmbeddedFonts[]; // fonts_data.cpp: Sans Regular, Sans Bold, Mono Regular, Mono Bold

static bool registerFonts() {
  int ok = 0;
  for (const EmbeddedFont *f = kEmbeddedFonts; f->data; f++) {
#ifdef _WIN32
    DWORD n = 0;
    if (AddFontMemResourceEx((PVOID)f->data, (DWORD)f->size, NULL, &n) && n) ok++;
#else
    CFDataRef d = CFDataCreate(NULL, f->data, (CFIndex)f->size);
    if (!d) continue;
    CGDataProviderRef p = CGDataProviderCreateWithCFData(d);
    CGFontRef cg = p ? CGFontCreateWithDataProvider(p) : NULL;
    if (cg) {
      CFErrorRef err = NULL;
      // already registered (another OLAAF instance in this process) counts as success
      if (CTFontManagerRegisterGraphicsFont(cg, &err)) ok++;
      else if (err) {
        if (CFErrorGetCode(err) == kCTFontManagerErrorAlreadyRegistered) ok++;
        CFRelease(err);
      }
      CGFontRelease(cg);
    }
    if (p) CGDataProviderRelease(p);
    CFRelease(d);
#endif
  }
  return ok == 4;
}
#endif

const char *uiFontFace(bool mono) {
#if defined(_WIN32) || defined(__APPLE__)
  static const bool embedded = registerFonts();
  if (embedded) return mono ? "IBM Plex Mono" : "IBM Plex Sans";
#endif
#ifdef _WIN32
  return mono ? "Consolas" : "Segoe UI";
#elif defined(__APPLE__)
  return mono ? "Menlo" : "Helvetica Neue";
#else
  return mono ? "Monospace" : "Sans";
#endif
}

} // namespace olaaf
