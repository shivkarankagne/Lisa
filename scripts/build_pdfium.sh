#!/bin/bash
# SPDX-License-Identifier: BUSL-1.1
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
    Darwin)              OS=mac ;;
    Linux)               OS=linux ;;
    MINGW*|MSYS*|CYGWIN*) OS=win ;;   # Git Bash on the Windows runner
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

# On Windows, use the locally installed Visual Studio toolchain instead of
# Google's internal package (which is not accessible to us). Chromium's
# setup_toolchain.py then needs to be told where VS is.
if [ "$OS" = "win" ]; then
    export DEPOT_TOOLS_WIN_TOOLCHAIN=0
    VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    VS_PATH="$("$VSWHERE" -latest -products '*' -property installationPath)"
    if [ -z "$VS_PATH" ]; then
        echo "error: Visual Studio not found via vswhere" >&2
        exit 1
    fi
    export GYP_MSVS_OVERRIDE_PATH="$VS_PATH"
    export GYP_MSVS_VERSION=2022
    export vs2022_install="$VS_PATH"
    echo "using Visual Studio at: $VS_PATH"
    LIB_NAME="pdfium.lib"
else
    LIB_NAME="libpdfium.a"
fi

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
# Called by absolute path: the script locates its own directory from $0.
if [ "$OS" = "win" ]; then
    # Windows: win_tools.bat installs depot_tools' bundled git, python,
    # gn and ninja. Without it, gclient's git subprocess is not found
    # (WinError 2). The .bat wrappers below use these bundled tools.
    cmd //c "$(cygpath -w "$WORK/depot_tools/bootstrap/win_tools.bat")"
    GCLIENT="gclient.bat"; GN="gn.bat"; NINJA="ninja.bat"
else
    if [ ! -f "$WORK/depot_tools/python3_bin_reldir.txt" ]; then
        "$WORK/depot_tools/ensure_bootstrap"
    fi
    if [ ! -f "$WORK/depot_tools/python3_bin_reldir.txt" ]; then
        echo "error: depot_tools bootstrap failed (python3_bin_reldir.txt missing)" >&2
        exit 1
    fi
    GCLIENT="gclient"; GN="gn"; NINJA="ninja"
fi
export DEPOT_TOOLS_UPDATE=0

if [ ! -f .gclient ]; then
    "$GCLIENT" config --unmanaged https://pdfium.googlesource.com/pdfium.git \
        --custom-var checkout_configuration=minimal
fi
"$GCLIENT" sync --no-history --shallow --revision "pdfium@$PDFIUM_COMMIT"

cd pdfium

# Windows: Chromium pins a specific Windows SDK (e.g. 10.0.28000.0, a
# preview) that the runner does not have. Pin it to the newest SDK that is
# actually installed, both through the toolchain env and by rewriting the
# version wherever the fetched build scripts hardcode it. This is the same
# kind of allowed edit to fetched third-party build files as the CREL strip.
if [ "$OS" = "win" ]; then
    SDK_INC="/c/Program Files (x86)/Windows Kits/10/Include"
    INSTALLED_SDK=$(ls "$SDK_INC" 2>/dev/null | grep -E '^10\.' | sort -V | tail -1)
    if [ -z "$INSTALLED_SDK" ]; then
        echo "error: no Windows 10 SDK found under $SDK_INC" >&2
        exit 1
    fi
    echo "pinning Windows SDK to installed version: $INSTALLED_SDK"
    export WINDOWSSDKVERSION="$INSTALLED_SDK"
    export WindowsSdkVerBinPath="/c/Program Files (x86)/Windows Kits/10/bin/$INSTALLED_SDK/"
    # Rewrite every pinned SDK version that is not the installed one. Chromium
    # hardcodes its desired version (a preview like 10.0.28000.0) in several
    # build files; point them all at the SDK the runner actually has.
    for V in $(grep -rhoE '10\.0\.[0-9]{5}\.0' build/ 2>/dev/null | sort -u); do
        [ "$V" = "$INSTALLED_SDK" ] && continue
        echo "rewriting Chromium SDK $V -> $INSTALLED_SDK in:"
        grep -rlE "$V" build/ 2>/dev/null | tee /dev/stderr \
            | xargs -r sed -i "s/${V//./\\.}/$INSTALLED_SDK/g"
    done
fi

# Chromium enables CREL (compact) relocations on Linux x64 by passing
# -Wa,--crel,--allow-experimental-crel to the assembler, but only because
# it links with its own lld. CREL is a 2024 format the host linkers here
# do not handle, so LISA cannot link the resulting libpdfium.a. lld is
# required for Chromium's own build (its bundled sysroot needs it), so
# rather than disable lld the flag itself is removed from the fetched
# build config; the objects then use ordinary relocations.
find build -name '*.gn' -o -name '*.gni' 2>/dev/null \
    | xargs grep -l -- '--allow-experimental-crel' 2>/dev/null \
    | xargs -r sed -i '/--allow-experimental-crel/d'

"$GN" gen out/lisa --args="
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
    use_thin_lto=false
    is_cfi=false
    use_allocator_shim=false
    use_partition_alloc_as_malloc=false
"
# Thin-LTO (Chromium's default) leaves LLVM bitcode in the object files,
# which the LLVM linker can read but GNU ld (the default on Linux) cannot
# ("unknown architecture of input file"). Disabling it, and CFI which
# depends on it, makes libpdfium.a a plain native archive that any linker
# accepts. It also keeps the two platforms' archives built the same way.
#
# use_allocator_shim / use_partition_alloc_as_malloc: Chromium's
# PartitionAlloc otherwise overrides global malloc/free through static
# initializers, which crashes any non-Chromium program that links PDFium
# — every LISA binary segfaulted at startup on Linux until these were
# turned off. PDFium then uses the system allocator.
#
# The CREL relocations are stripped above (see the sed on the fetched
# build config): Chromium adds them on Linux x64 under lld, a 2024 format
# GNU ld, mold and lld here cannot all handle, so LISA could not link the
# archive. Removing the flag while keeping lld (which Chromium's own build
# needs) leaves ordinary relocations that any linker reads. ARM is
# excluded upstream, which is why macOS was unaffected.
"$NINJA" -C out/lisa pdfium

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include"
cp "out/lisa/obj/$LIB_NAME" "$OUT/lib/"
cp -R public/. "$OUT/include/"
cp LICENSE "$OUT/LICENSE"
cat > "$OUT/VERSION" <<VER
branch=$PDFIUM_BRANCH
commit=$PDFIUM_COMMIT
os=$OS
cpu=$CPU
VER
echo "PDFium built: $OUT/lib/$LIB_NAME ($(du -h "$OUT/lib/$LIB_NAME" | cut -f1))"
