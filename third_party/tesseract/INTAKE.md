# Tesseract (vendored)

- Upstream: https://github.com/tesseract-ocr/tesseract
- Version: 5.5.0 (tag 5.5.0)
- Purpose: OCR engine for scanned PDF pages on Linux/Windows.
- Trimmed to: src/, include/, cmake/, CMakeLists.txt, VERSION, LICENSE,
  tesseract.pc.cmake. Dropped doc/, java/, test/, unittest/, tessdata/,
  nsis/, snap/, m4/.
- Built in-tree via add_subdirectory with: DISABLED_LEGACY_ENGINE=ON
  (LSTM only), BUILD_TRAINING_TOOLS=OFF, BUILD_TESTS=OFF, DISABLE_CURL/
  ARCHIVE/TIFF=ON, GRAPHICS_DISABLED=ON, SW_BUILD=OFF, USE_SYSTEM_ICU=OFF.
- LOCAL EDIT (CMakeLists.txt, Leptonica block): added a LISA_LEPTONICA_TARGET
  branch so Tesseract uses LISA's in-tree Leptonica target instead of
  find_package, and skips the TIFF try_compile probe. Documented inline.
- Not built on macOS (Apple Vision is the OCR backend there).
