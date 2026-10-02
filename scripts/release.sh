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
# Per-OS: binary name, archive format, and the extra CMake flags a release
# build needs. Windows must use Clang (MSVC ABI) + Ninja, exactly like CI.
CMAKE_EXTRA=""
BINEXT=""
ARCHIVE=tar.gz
case "$(uname -s)" in
    Darwin) OS=macos; NPROC=$(sysctl -n hw.ncpu); FILESIZE='stat -f%z' ;;
    Linux)  OS=linux; NPROC=$(nproc);             FILESIZE='stat -c%s' ;;
    MINGW*|MSYS*|CYGWIN*)
        OS=windows; NPROC=$(nproc); FILESIZE='stat -c%s'
        BINEXT=.exe; ARCHIVE=zip
        CMAKE_EXTRA="-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++" ;;
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
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release $CMAKE_EXTRA >/dev/null
cmake --build build-release -j"$NPROC" >/dev/null

echo "==> Checking the binary"
BIN="build-release/lisa$BINEXT"
"$BIN" --version

# Nothing but the operating system's own libraries may be linked.
if [ "$OS" = macos ]; then
    if otool -L "$BIN" | tail -n +2 | grep -vE '^\s+(/usr/lib/|/System/Library/)'; then
        echo "error: $BIN links a non-system library" >&2
        exit 1
    fi
elif [ "$OS" = linux ]; then
    # Allow only the base C/C++ runtime and the loader.
    if ldd "$BIN" | grep -oE '[a-zA-Z0-9_+-]+\.so[.0-9]*' | sort -u \
        | grep -vE '^(libc|libm|libdl|librt|libpthread|libstdc\+\+|libgcc_s|ld-linux[a-z0-9-]*|linux-vdso)\.so' ; then
        echo "error: $BIN links a non-system shared library (see above)" >&2
        exit 1
    fi
else
    # Windows: every import is a system DLL by design — the static CRT (/MT),
    # static PDFium, webview's built-in WebView2 loader, and the WinRT/Win32
    # umbrella libs. Flag any non-system DLL import. dumpbin (from the MSVC
    # toolchain) lists them; if it isn't on PATH, fall back to --version only.
    if command -v dumpbin >/dev/null 2>&1; then
        if dumpbin //dependents "$BIN" | grep -iE '\.dll$' \
            | grep -viE '\s+(KERNEL32|KERNELBASE|ntdll|USER32|GDI32|ADVAPI32|ole32|OLEAUT32|SHELL32|SHLWApi|VERSION|WS2_32|bcrypt|CRYPT32|RPCRT4|combase|WINDOWSAPP|api-ms-win-[a-z0-9.-]+|msvcrt|ucrtbase|sechost|IMM32|dwmapi)\.dll' ; then
            echo "error: $BIN imports a non-system DLL (see above)" >&2
            exit 1
        fi
    else
        echo "    (dumpbin not found; skipped the import allowlist, ran --version only)"
    fi
fi
echo "    size: $($FILESIZE "$BIN") bytes"

echo "==> Tests (model tests need models/; the GUI test needs a browser, covered in CI)"
ctest --test-dir build-release --output-on-failure -E test_gui

echo "==> Packaging"
ARCHIVE_FILE="$NAME.$ARCHIVE"
rm -rf dist "$NAME"
mkdir -p "dist/$NAME"
cp "$BIN" "dist/$NAME/lisa$BINEXT"
cp README.md LICENSE NOTICE SECURITY.md "dist/$NAME/"
mkdir -p "dist/$NAME/docs"
cp docs/models.md docs/http-api.md "dist/$NAME/docs/"
cp src/cli/README.md "dist/$NAME/docs/cli.md"
if [ "$ARCHIVE" = zip ]; then
    # Prefer 7z, then PowerShell, then bsdtar — one is always present on the
    # Windows runner.
    ( cd dist && {
        7z a -tzip -bso0 -bsp0 "$ARCHIVE_FILE" "$NAME" >/dev/null 2>&1 \
        || powershell -NoProfile -Command "Compress-Archive -Force -Path '$NAME' -DestinationPath '$ARCHIVE_FILE'" \
        || tar -a -c -f "$ARCHIVE_FILE" "$NAME"; } )
else
    ( cd dist && tar czf "$ARCHIVE_FILE" "$NAME" )
fi
( cd dist && rm -rf "$NAME" )
( cd dist && sha256 "$ARCHIVE_FILE" > "$ARCHIVE_FILE.sha256" )
sha256 "dist/$ARCHIVE_FILE"

if [ "$PUBLISH" = "1" ]; then
    echo "==> Publishing (attaching $OS/$ARCH assets to release v$VERSION)"
    if [ "$OS" = macos ]; then
        FIRSTRUN="The binary is not signed yet: the first time, right-click it in Finder and choose Open."
        PLATLINE="Apple Silicon Macs (macOS 13+). One file, no dependencies."
        UNPACK="tar xzf $ARCHIVE_FILE && cd $NAME && ./lisa --version"
    elif [ "$OS" = linux ]; then
        FIRSTRUN="A static binary; no dependencies to install."
        PLATLINE="x86-64 Linux (glibc). One file."
        UNPACK="tar xzf $ARCHIVE_FILE && cd $NAME && ./lisa --version"
    else
        FIRSTRUN="One .exe, no install. Windows 10/11 x64; the WebView2 runtime (for the GUI) ships with current Windows."
        PLATLINE="x86-64 Windows 10/11. One file."
        UNPACK="Expand the zip, then: cd $NAME && lisa.exe --version"
    fi
    NOTES=$(cat <<NOTES
$PLATLINE

    $UNPACK

$FIRSTRUN See README.md for the quickstart.
NOTES
)
    # Create the release once; later platform runs just upload their assets.
    if gh release view "v$VERSION" >/dev/null 2>&1; then
        gh release upload "v$VERSION" "dist/$ARCHIVE_FILE" "dist/$ARCHIVE_FILE.sha256" --clobber
    else
        gh release create "v$VERSION" "dist/$ARCHIVE_FILE" "dist/$ARCHIVE_FILE.sha256" \
            --title "LISA $VERSION" --notes "$NOTES"
    fi
else
    echo "==> Not published. To publish: scripts/release.sh --publish"
fi
