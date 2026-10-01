#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
#
# Download LISA's prebuilt static PDFium, verify it, and unpack it into
# .deps/pdfium. This is the normal way to get PDFium; scripts/build_pdfium.sh
# is only needed to upgrade PDFium or to reproduce the prebuilt archive.
#
# The archive is attached to a GitHub Release of the LISA repository.
# Private repositories need `gh auth login` (or GH_TOKEN in CI).
#
# Usage: scripts/fetch_pdfium.sh [output_dir]

set -euo pipefail

COMMIT="a5a7089234f121990b336b3841008009dca143bf"
REPO="${LISA_DEPS_REPO:-shivkarankagne/Lisa}"

# The prebuilt static library per platform. Each is built from the same
# pinned commit by scripts/build_pdfium.sh and published as a release
# asset (macOS by hand, Linux by build-pdfium-linux.yml).
LIB="libpdfium.a"
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)
        ASSET="pdfium-chromium-8057-mac-arm64.tar.gz"
        SHA256="07ae3e816fee0626ffd1c31cc3b10be3bbadccad3eb3033691b777d1dd5c0ba9"
        ;;
    Linux/x86_64)
        ASSET="pdfium-chromium-8057-linux-x64.tar.gz"
        SHA256="e1200afa122fcef4e054d1e173d307b90f1f2c2b7559efa4a7a9c7558b56a1a9"
        ;;
    MINGW*/x86_64|MSYS*/x86_64|CYGWIN*/x86_64)
        ASSET="pdfium-chromium-8057-windows-x64.tar.gz"
        SHA256="4aa7cd4fa6d74e90e2d0ef93c0a3a66832713ea23774cfc2fb2e0aa392f26448"
        LIB="pdfium.lib"
        ;;
    *)
        echo "error: no prebuilt PDFium for $(uname -s)/$(uname -m); run scripts/build_pdfium.sh" >&2
        exit 1
        ;;
esac
TAG="deps-${ASSET%.tar.gz}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.deps/pdfium}"

if [ -f "$OUT/lib/$LIB" ] && grep -q "commit=$COMMIT" "$OUT/VERSION" 2>/dev/null; then
    echo "PDFium already present: $OUT"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if command -v gh >/dev/null 2>&1; then
    gh release download "$TAG" --repo "$REPO" --pattern "$ASSET" --dir "$TMP"
else
    curl -fsSL --retry 5 -o "$TMP/$ASSET" \
        "https://github.com/$REPO/releases/download/$TAG/$ASSET"
fi

# shasum on macOS; sha256sum on Linux and Git Bash (Windows).
if command -v shasum >/dev/null 2>&1; then
    echo "$SHA256  $TMP/$ASSET" | shasum -a 256 -c -
else
    echo "$SHA256  $TMP/$ASSET" | sha256sum -c -
fi

tar -C "$TMP" -xzf "$TMP/$ASSET"
rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
mv "$TMP/${ASSET%.tar.gz}" "$OUT"
echo "PDFium ready: $OUT"
cat "$OUT/VERSION"
