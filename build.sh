#!/bin/bash
# Rebuilds everything in prebuilt/ from the official SDL and libusb sources.
# Not needed to install — install.sh uses prebuilt/. Requires Xcode command
# line tools, cmake and pkg-config (e.g. `brew install cmake pkg-config`).
set -euo pipefail

SDL_VERSION="2.32.10"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
SDL_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
LIBUSB_VERSION="1.0.29"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v$LIBUSB_VERSION/libusb-$LIBUSB_VERSION.tar.bz2"
LIBUSB_SHA256="5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85"

HERE="$(cd "$(dirname "$0")" && pwd)"
# A fixed, neutral path: SDL embeds source paths in its assertion messages.
B="/tmp/xboxoneformac-build"
JOBS="$(sysctl -n hw.ncpu)"

for tool in cmake pkg-config clang; do
  command -v "$tool" >/dev/null || { echo "Missing $tool (brew install cmake pkg-config)." >&2; exit 1; }
done

fetch() { # url sha256 file
  [ -f "$3" ] || curl -fL --retry 3 -o "$3" "$1"
  [ "$(shasum -a 256 "$3" | cut -d' ' -f1)" = "$2" ] || { echo "checksum mismatch: $3" >&2; rm -f "$3"; exit 1; }
}

rm -rf "$B"; mkdir -p "$B/dl"
fetch "$SDL_URL" "$SDL_SHA256" "$B/dl/sdl.tar.gz"
fetch "$LIBUSB_URL" "$LIBUSB_SHA256" "$B/dl/libusb.tar.bz2"

sdl_tree() { # dir patch
  mkdir -p "$1"; tar -xzf "$B/dl/sdl.tar.gz" -C "$1" --strip-components 1
  patch -d "$1" -p1 --quiet < "$2"
}

# ---- libusb (arm64), used by the bridge service
mkdir -p "$B/libusb"; tar -xjf "$B/dl/libusb.tar.bz2" -C "$B/libusb" --strip-components 1
( cd "$B/libusb"
  ./configure --quiet --host=aarch64-apple-darwin --prefix="$B/usr" --disable-static \
    CFLAGS="-arch arm64 -mmacosx-version-min=11.0 -O2" LDFLAGS="-arch arm64"
  make -s -j"$JOBS" && make -s install )
install_name_tool -id @rpath/libusb-1.0.0.dylib "$B/usr/lib/libusb-1.0.0.dylib"

# ---- SDL for the bridge service (arm64): libusb backend for the wired Xbox One
sdl_tree "$B/sdl-bridge" "$HERE/patches/sdl-bridge-libusb.patch"
PKG_CONFIG_PATH="$B/usr/lib/pkgconfig" cmake -S "$B/sdl-bridge" -B "$B/sdl-bridge/build" -Wno-dev \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=ON >/dev/null
grep -q '#define HAVE_LIBUSB 1' "$B/sdl-bridge/build/include-config-release/SDL2/SDL_config.h" \
  || { echo "SDL was configured without libusb" >&2; exit 1; }
cmake --build "$B/sdl-bridge/build" -j"$JOBS" >/dev/null

# ---- SDL for Wine (x86_64): stock SDL plus the bridge driver
sdl_tree "$B/sdl-wine" "$HERE/patches/sdl-wine-bridge.patch"
cp "$HERE/src/SDL_xboxonebridge.inc" "$HERE/src/bridge_protocol.h" "$B/sdl-wine/src/joystick/"
cmake -S "$B/sdl-wine" -B "$B/sdl-wine/build" -Wno-dev \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_HIDAPI=ON -DSDL_HIDAPI_LIBUSB=OFF >/dev/null
cmake --build "$B/sdl-wine/build" -j"$JOBS" >/dev/null

# ---- bridge service (arm64)
mkdir -p "$B/out"
cp "$B/usr/lib/libusb-1.0.0.dylib" "$B/out/libusb-1.0.0.dylib"
cp -L "$B/sdl-bridge/build/libSDL2-2.0.0.dylib" "$B/out/libSDL2-2.0.0.dylib"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$B/out/libSDL2-2.0.0.dylib"
clang -arch arm64 -mmacosx-version-min=11.0 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I"$B/sdl-bridge/include" -I"$B/sdl-bridge/build/include-config-release/SDL2" -I"$B/usr/include/libusb-1.0" \
  -I"$HERE/src" "$HERE/src/xboxone-bridge.c" -o "$B/out/xboxone-bridge" \
  -L"$B/out" -lSDL2-2.0.0 -lusb-1.0.0 -framework IOKit -framework CoreFoundation -Wl,-rpath,@executable_path

# ---- Wine-side SDL + its test (x86_64, runs under Rosetta)
cp -L "$B/sdl-wine/build/libSDL2-2.0.0.dylib" "$B/out/wine-libSDL2-2.0.0.dylib"
mkdir -p "$B/test"; cp "$B/out/wine-libSDL2-2.0.0.dylib" "$B/test/libSDL2-2.0.0.dylib"
clang -arch x86_64 -mmacosx-version-min=10.15 -O1 -I"$B/sdl-wine/include" \
  -I"$B/sdl-wine/build/include-config-release/SDL2" -I"$HERE/src" "$HERE/src/test_bridge.c" \
  -o "$B/test/test_bridge" -L"$B/test" -lSDL2-2.0.0 -Wl,-rpath,@executable_path
"$B/test/test_bridge"

# ---- strip debug info (keeps exports) and sign ad hoc
for f in "$B/out/"*; do
  strip -S -x "$f" 2>/dev/null || strip -S "$f"
  codesign --force --sign - "$f" >/dev/null
done

mkdir -p "$HERE/prebuilt/bridge" "$HERE/prebuilt/wine"
cp "$B/out/xboxone-bridge" "$B/out/libSDL2-2.0.0.dylib" "$B/out/libusb-1.0.0.dylib" "$HERE/prebuilt/bridge/"
cp "$B/out/wine-libSDL2-2.0.0.dylib" "$HERE/prebuilt/wine/libSDL2-2.0.0.dylib"
( cd "$HERE/prebuilt" && shasum -a 256 bridge/* wine/* > SHA256SUMS )
echo "prebuilt/ updated:"; cat "$HERE/prebuilt/SHA256SUMS"
