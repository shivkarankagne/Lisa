#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
# release.sh — build the release binary, check it, and package it with
# checksums (plan §6 W11).
#
#   scripts/release.sh            build and package into dist/
#   scripts/release.sh --publish  also create the GitHub release (needs gh)
#
# Signing and notarisation are not done here yet: they need an Apple
# Developer ID. Until then macOS asks the user to confirm the first run.
set -euo pipefail
cd "$(dirname "$0")/.."

PUBLISH=0
[ "${1:-}" = "--publish" ] && PUBLISH=1

VERSION=$(sed -n 's/^#define LISA_VERSION_STRING "\(.*\)"/\1/p' include/lisa.h)
[ -n "$VERSION" ] || { echo "cannot read the version from include/lisa.h" >&2; exit 1; }
ARCH=$(uname -m)
NAME="lisa-$VERSION-macos-$ARCH"
echo "==> LISA $VERSION ($ARCH)"

if [ -n "$(git status --porcelain)" ]; then
    echo "warning: the working tree has uncommitted changes" >&2
fi

echo "==> Building"
[ -x .deps/pdfium/lib/libpdfium.a ] || scripts/fetch_pdfium.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release -j"$(sysctl -n hw.ncpu)" >/dev/null

echo "==> Checking the binary"
BIN=build-release/lisa
"$BIN" --version
# Only system libraries and frameworks may be linked.
if otool -L "$BIN" | tail -n +2 | grep -vE '^\s+(/usr/lib/|/System/Library/)'; then
    echo "error: $BIN links a non-system library" >&2
    exit 1
fi
echo "    size: $(stat -f%z "$BIN") bytes"

echo "==> Tests (model tests need models/)"
ctest --test-dir build-release --output-on-failure >/dev/null

echo "==> Packaging"
rm -rf dist "$NAME"
mkdir -p "dist/$NAME"
cp "$BIN" "dist/$NAME/lisa"
cp README.md LICENSE NOTICE SECURITY.md "dist/$NAME/"
mkdir -p "dist/$NAME/docs"
cp docs/models.md docs/http-api.md "dist/$NAME/docs/"
cp src/cli/README.md "dist/$NAME/docs/cli.md"
( cd dist && tar czf "$NAME.tar.gz" "$NAME" && rm -rf "$NAME" )
( cd dist && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
shasum -a 256 "dist/$NAME.tar.gz"

if [ "$PUBLISH" = "1" ]; then
    echo "==> Publishing v$VERSION"
    gh release create "v$VERSION" "dist/$NAME.tar.gz" "dist/$NAME.tar.gz.sha256" \
        --title "LISA $VERSION" --notes-file <(cat <<NOTES
Apple Silicon Macs (macOS 13+). One file, no dependencies.

    shasum -a 256 -c $NAME.tar.gz.sha256
    tar xzf $NAME.tar.gz && cd $NAME && ./lisa --version

The binary is not signed yet: the first time, right-click it in Finder
and choose Open. See README.md for the quickstart.
NOTES
)
else
    echo "==> Not published. To publish: scripts/release.sh --publish"
fi
