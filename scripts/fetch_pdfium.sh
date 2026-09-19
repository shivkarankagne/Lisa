#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
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

TAG="deps-pdfium-chromium-8057-mac-arm64"
ASSET="pdfium-chromium-8057-mac-arm64.tar.gz"
SHA256="07ae3e816fee0626ffd1c31cc3b10be3bbadccad3eb3033691b777d1dd5c0ba9"
COMMIT="a5a7089234f121990b336b3841008009dca143bf"
REPO="${LISA_DEPS_REPO:-shivkarankagne/Lisa}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.deps/pdfium}"

if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
    echo "error: no prebuilt PDFium for $(uname -s)/$(uname -m); run scripts/build_pdfium.sh" >&2
    exit 1
fi

if [ -f "$OUT/lib/libpdfium.a" ] && grep -q "commit=$COMMIT" "$OUT/VERSION" 2>/dev/null; then
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

echo "$SHA256  $TMP/$ASSET" | shasum -a 256 -c -

tar -C "$TMP" -xzf "$TMP/$ASSET"
rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
mv "$TMP/${ASSET%.tar.gz}" "$OUT"
echo "PDFium ready: $OUT"
cat "$OUT/VERSION"
