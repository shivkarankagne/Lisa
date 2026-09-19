#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a static PDFium library for LISA (plan W5; third_party/pdfium/INTAKE.md).
#
# PDFium publishes no static macOS library, so LISA builds one from a pinned
# commit with Google's build tools. The result is not committed; CMake finds
# it in .deps/pdfium (override with -DLISA_PDFIUM_DIR=...).
#
# Usage: scripts/build_pdfium.sh [output_dir]
#
# Needs: git, python3, ~5 GB free disk, network access. First run takes
# a while (source checkout + compile); later runs reuse the checkout.

set -euo pipefail

PDFIUM_BRANCH="chromium/8057"
PDFIUM_COMMIT="a5a7089234f121990b336b3841008009dca143bf"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.deps/pdfium}"
WORK="${PDFIUM_WORK:-$ROOT/.deps/pdfium-src}"

case "$(uname -m)" in
    arm64|aarch64) CPU=arm64 ;;
    x86_64)        CPU=x64 ;;
    *) echo "unsupported CPU: $(uname -m)" >&2; exit 1 ;;
esac
case "$(uname -s)" in
    Darwin) OS=mac ;;
    Linux)  OS=linux ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d depot_tools ]; then
    git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
# .cipd_bin holds helper binaries depot_tools installs for itself (luci-auth...).
export PATH="$WORK/depot_tools:$WORK/depot_tools/.cipd_bin:$PATH"
export DEPOT_TOOLS_METRICS=0
# One-time setup of the tools' own Python/CIPD environment (gn, ninja).
# After that, keep depot_tools pinned at the version we cloned.
if [ ! -f depot_tools/python3_bin_reldir.txt ]; then
    depot_tools/ensure_bootstrap
fi
export DEPOT_TOOLS_UPDATE=0

if [ ! -f .gclient ]; then
    gclient config --unmanaged https://pdfium.googlesource.com/pdfium.git \
        --custom-var checkout_configuration=minimal
fi
gclient sync --no-history --shallow --revision "pdfium@$PDFIUM_COMMIT"

cd pdfium
gn gen out/lisa --args="
    is_debug=false
    symbol_level=0
    target_os=\"$OS\"
    target_cpu=\"$CPU\"
    pdf_is_standalone=true
    pdf_is_complete_lib=true
    pdf_enable_v8=false
    pdf_enable_xfa=false
    pdf_use_skia=false
    is_component_build=false
    use_custom_libcxx=false
    clang_use_chrome_plugins=false
    treat_warnings_as_errors=false
    use_remoteexec=false
"
ninja -C out/lisa pdfium

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include"
cp out/lisa/obj/libpdfium.a "$OUT/lib/"
cp -R public/. "$OUT/include/"
cp LICENSE "$OUT/LICENSE"
cat > "$OUT/VERSION" <<VER
branch=$PDFIUM_BRANCH
commit=$PDFIUM_COMMIT
os=$OS
cpu=$CPU
VER
echo "PDFium built: $OUT/lib/libpdfium.a ($(du -h "$OUT/lib/libpdfium.a" | cut -f1))"
