#!/bin/bash
# olAAF - builds the REAPER extension on this Mac and installs it.
# Double-click in Finder. Needs Apple's free Command Line Tools (the script offers to install them).

cd "$(dirname "$0")" || exit 1
set -e

echo ""
echo "olAAF - Do you wanna build a Reaper Session?"
echo "--------------------------------------------"

OS=$(uname)
if [ "$OS" = "Darwin" ]; then
  if ! xcode-select -p >/dev/null 2>&1; then
    echo "Apple Command Line Tools are needed once. A window will open: click 'Install'."
    echo "When it is finished, double-click this file again."
    xcode-select --install || true
    read -r -p "Press Return to close."
    exit 0
  fi
  CC="clang"
  CXX="clang++"
  ARCH="-arch arm64 -arch x86_64 -mmacosx-version-min=10.15"
  OUT="reaper_olaaf.dylib"
  LINK="-dynamiclib -undefined dynamic_lookup"
  DEST="$HOME/Library/Application Support/REAPER/UserPlugins"
else
  # Linux (also used to test this script)
  CC="${CC:-cc}"
  CXX="${CXX:-c++}"
  ARCH="-fPIC"
  OUT="reaper_olaaf.so"
  LINK="-shared"
  DEST="$HOME/.config/REAPER/UserPlugins"
fi

B=build_local
rm -rf "$B"
mkdir -p "$B/obj"

echo "1/3  Compiling libAAF ..."
for f in third_party/libaaf/src/LibCFB/*.c third_party/libaaf/src/AAFCore/*.c \
         third_party/libaaf/src/AAFIface/*.c third_party/libaaf/src/common/*.c; do
  o="$B/obj/$(basename "$f" .c).o"
  $CC $ARCH -O2 -std=c99 -w -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=500 -D_DARWIN_C_SOURCE \
      -Ithird_party/libaaf/include -Ithird_party/libaaf/src -c "$f" -o "$o"
done

echo "2/3  Compiling olAAF ..."
for f in src/core/*.cpp src/ext/main.cpp src/ext/progress.cpp src/ext/dialog.cpp src/ext/theme.cpp src/ext/logo.cpp; do
  o="$B/obj/olaaf_$(basename "$f" .cpp).o"
  $CXX $ARCH -O2 -std=c++17 -fvisibility=hidden -DSWELL_PROVIDED_BY_APP \
       -Ithird_party/libaaf/include -Ithird_party/reaper-sdk/sdk -c "$f" -o "$o"
done

if [ "$OS" = "Darwin" ]; then
  $CXX $ARCH -O2 -x objective-c++ -DSWELL_PROVIDED_BY_APP -c third_party/reaper-sdk/WDL/swell/swell-modstub.mm -o "$B/obj/swell_modstub.o"
  LINK="$LINK -framework AppKit"
else
  $CXX $ARCH -O2 -DSWELL_PROVIDED_BY_APP -c third_party/reaper-sdk/WDL/swell/swell-modstub-generic.cpp -o "$B/obj/swell_modstub.o"
  LINK="$LINK -lpthread -ldl"
fi

echo "3/3  Linking ..."
$CXX $ARCH $LINK -o "$B/$OUT" "$B"/obj/*.o

mkdir -p "$DEST"
cp -f "$B/$OUT" "$DEST/"
echo ""
echo "Installed: $DEST/$OUT"
echo "Restart REAPER. Then use File > Open project (pick an .aaf), or the action"
echo "'olAAF: Import AAF into current project...'."
echo ""
if [ "$OS" = "Darwin" ]; then read -r -p "Press Return to close."; fi
