#!/usr/bin/env bash
set -euo pipefail

# Builds indexed-x86_64.AppImage from a release build, using linuxdeploy +
# linuxdeploy-plugin-qt (indexed-plan.md §14.2). Fetches both tools on first
# run (cached under packaging/appimage/tools/, gitignored) and requires
# network access for that; nothing else here touches the network.
#
# Usage: packaging/appimage/build-appimage.sh [build-dir]
#   build-dir defaults to build/release; configured with linux-gcc-release
#   if not already configured there.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build/release}"
APPIMAGE_DIR="$ROOT_DIR/packaging/appimage"
TOOLS_DIR="$APPIMAGE_DIR/tools"
APPDIR="$APPIMAGE_DIR/AppDir"
OUTPUT="$APPIMAGE_DIR/indexed-x86_64.AppImage"

mkdir -p "$TOOLS_DIR"

fetch_tool() {
    local name="$1" url="$2"
    local dest="$TOOLS_DIR/$name"
    if [[ ! -x "$dest" ]]; then
        echo "Fetching $name..."
        curl -fL --retry 3 -o "$dest" "$url"
        chmod +x "$dest"
    fi
}

fetch_tool linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch_tool linuxdeploy-plugin-qt \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

# linuxdeploy's bundled patchelf (0.15.0) predates RELR (.relr.dyn) sections
# and silently corrupts every library it rewrites on distros built with a
# modern binutils (same era of problem as the strip issue below) -- the
# resulting AppImage segfaults in random library constructors at startup.
# Fetch a RELR-aware patchelf and point linuxdeploy at it via $PATCHELF.
# (0.18.0 still corrupts RELR libs; 0.19.1 is the first release verified to
# produce loadable output on Fedora 44 -- keep this pinned >= 0.19.1.)
PATCHELF_VERSION=0.19.1
if [[ ! -x "$TOOLS_DIR/patchelf" ]]; then
    echo "Fetching patchelf $PATCHELF_VERSION..."
    curl -fL --retry 3 -o "$TOOLS_DIR/patchelf.tar.gz" \
        "https://github.com/NixOS/patchelf/releases/download/$PATCHELF_VERSION/patchelf-$PATCHELF_VERSION-x86_64.tar.gz"
    tar -xzf "$TOOLS_DIR/patchelf.tar.gz" -C "$TOOLS_DIR" --strip-components=2 ./bin/patchelf
    rm -f "$TOOLS_DIR/patchelf.tar.gz"
    chmod +x "$TOOLS_DIR/patchelf"
fi
export PATCHELF="$TOOLS_DIR/patchelf"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cmake --preset linux-gcc-release
fi
cmake --build "$BUILD_DIR" --target indexed indexed-helper --parallel

rm -rf "$APPDIR"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
# linuxdeploy's bundled `strip` predates RELR (.relr.dyn) sections and rejects
# libs built on a newer distro's binutils; skip stripping rather than fail.
export NO_STRIP=1

rm -f "$APPIMAGE_DIR"/indexed-*.AppImage
(
    cd "$APPIMAGE_DIR"
    "$TOOLS_DIR/linuxdeploy" \
        --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/indexed-helper" \
        --desktop-file "$APPDIR/usr/share/applications/indexed.desktop" \
        --icon-file "$ROOT_DIR/packaging/icons/indexed.png" \
        --plugin qt \
        --output appimage
)

for f in "$APPIMAGE_DIR"/indexed-*.AppImage; do
    [[ "$f" == "$OUTPUT" ]] || mv "$f" "$OUTPUT"
done
echo "Built $OUTPUT"
