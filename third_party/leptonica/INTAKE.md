# Leptonica (vendored)

- Upstream: https://github.com/DanBloomberg/leptonica
- Version: 1.85.0 (tag 1.85.0)
- Purpose: image buffer handling (PIX) for Tesseract OCR on Linux/Windows.
- Trimmed to: src/, cmake/, CMakeLists.txt, lept.pc.cmake, license.
  Dropped prog/ (demos), docs, images.
- Built in-tree via add_subdirectory with ALL image-format backends OFF
  (ENABLE_ZLIB/PNG/GIF/JPEG/TIFF/WEBP/OPENJPEG=OFF, BUILD_PROG=OFF,
  SW_BUILD=OFF): LISA feeds raw BGRx pixels, so no codecs are needed.
- Not built on macOS (Apple Vision is the OCR backend there).
