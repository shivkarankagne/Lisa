#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
# release.sh — build the release binary, check it, and package it with
# checksums (plan §6 W11). Works on macOS (arm64) and Linux (x86-64).
#
#   scripts/release.sh            build and package into dist/
#   scripts/release.sh --publish  also create the GitHub release (needs gh)
#
# On macOS, signing and notarisation are not done yet (they need an Apple
# Developer ID), so the first run must be confirmed in Finder. On Linux,
# building needs a linker that can read the PDFium objects (mold or lld);
# CMake picks one up automatically when installed.
set -euo pipefail
cd "$(dirname "$0")/.."

PUBLISH=0
[ "${1:-}" = "--publish" ] && PUBLISH=1

VERSION=$(sed -n 's/^#define LISA_VERSION_STRING "\(.*\)"/\1/p' include/lisa.h)
[ -n "$VERSION" ] || { echo "cannot read the version from include/lisa.h" >&2; exit 1; }

ARCH=$(uname -m)
case "$(uname -s)" in
    Darwin) OS=macos; NPROC=$(sysctl -n hw.ncpu); FILESIZE='stat -f%z' ;;
    Linux)  OS=linux; NPROC=$(nproc);             FILESIZE='stat -c%s' ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
NAME="lisa-$VERSION-$OS-$ARCH"
echo "==> LISA $VERSION ($OS/$ARCH)"

sha256() {  # portable SHA-256 of a file -> "<hash>  <file>"
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1"
    else sha256sum "$1"; fi
}

if [ -n "$(git status --porcelain)" ]; then
    echo "warning: the working tree has uncommitted changes" >&2
fi

echo "==> Building"
[ -f .deps/pdfium/lib/libpdfium.a ] || scripts/fetch_pdfium.sh
rm -rf build-release   # a release is always a clean build
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j"$NPROC" >/dev/null

echo "==> Checking the binary"
BIN=build-release/lisa
"$BIN" --version

# Nothing but the operating system's own libraries may be linked.
if [ "$OS" = macos ]; then
    if otool -L "$BIN" | tail -n +2 | grep -vE '^\s+(/usr/lib/|/System/Library/)'; then
        echo "error: $BIN links a non-system library" >&2
        exit 1
    fi
else
    # Allow only the base C/C++ runtime and the loader.
    if ldd "$BIN" | grep -oE '[a-zA-Z0-9_+-]+\.so[.0-9]*' | sort -u \
        | grep -vE '^(libc|libm|libdl|librt|libpthread|libstdc\+\+|libgcc_s|ld-linux[a-z0-9-]*|linux-vdso)\.so' ; then
        echo "error: $BIN links a non-system shared library (see above)" >&2
        exit 1
    fi
fi
echo "    size: $($FILESIZE "$BIN") bytes"

echo "==> Tests (model tests need models/; the GUI test needs a browser, covered in CI)"
ctest --test-dir build-release --output-on-failure -E test_gui >/dev/null

echo "==> Packaging"
rm -rf dist "$NAME"
mkdir -p "dist/$NAME"
cp "$BIN" "dist/$NAME/lisa"
cp README.md LICENSE NOTICE SECURITY.md "dist/$NAME/"
mkdir -p "dist/$NAME/docs"
cp docs/models.md docs/http-api.md "dist/$NAME/docs/"
cp src/cli/README.md "dist/$NAME/docs/cli.md"
( cd dist && tar czf "$NAME.tar.gz" "$NAME" && rm -rf "$NAME" )
( cd dist && sha256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
sha256 "dist/$NAME.tar.gz"

if [ "$PUBLISH" = "1" ]; then
    echo "==> Publishing (attaching $OS/$ARCH assets to release v$VERSION)"
    if [ "$OS" = macos ]; then
        FIRSTRUN="The binary is not signed yet: the first time, right-click it in Finder and choose Open."
        PLATLINE="Apple Silicon Macs (macOS 13+). One file, no dependencies."
    else
        FIRSTRUN="A static binary; no dependencies to install."
        PLATLINE="x86-64 Linux (glibc). One file."
    fi
    NOTES=$(cat <<NOTES
$PLATLINE

    tar xzf $NAME.tar.gz && cd $NAME && ./lisa --version

$FIRSTRUN See README.md for the quickstart.
NOTES
)
    # Create the release once; later platform runs just upload their assets.
    if gh release view "v$VERSION" >/dev/null 2>&1; then
        gh release upload "v$VERSION" "dist/$NAME.tar.gz" "dist/$NAME.tar.gz.sha256" --clobber
    else
        gh release create "v$VERSION" "dist/$NAME.tar.gz" "dist/$NAME.tar.gz.sha256" \
            --title "LISA $VERSION" --notes "$NOTES"
    fi
else
    echo "==> Not published. To publish: scripts/release.sh --publish"
fi
