/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * platform_ocr_windows.cpp — OCR backend for Windows via Windows.Media.Ocr
 * (C++/WinRT). This mirrors the macOS Vision backend (platform_macos.m): the
 * caller hands us a rasterised page as 32-bit BGRx rows and we return one line
 * of recognised text per line, as malloc'd UTF-8.
 *
 * Windows.Media.Ocr is a system recogniser (no bundled model, like Vision),
 * but it needs an OCR language pack. That is present by default on consumer
 * Windows 10/11 with English; when it is missing (N/KN editions without the
 * Media Feature Pack, some Server/LTSC SKUs, English-less installs) we report
 * "no text" and print a one-time hint on how to enable it.
 *
 * Linux, which has no system recogniser, uses Tesseract instead
 * (platform_ocr_tesseract.c). Tesseract is not used on Windows: its heavy
 * <string_view> use trips a clang-20 <mmintrin.h> bug on the MSVC toolchain.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

/* Make ::IUnknown a complete type before the C++/WinRT headers, so that
 * reference.as<IMemoryBufferByteAccess>() below resolves to the classic-COM
 * QueryInterface path rather than WinRT's value-boxing path. */
#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

extern "C" {
#include "platform.h"
}

/* Byte access to a WinRT memory buffer (standard COM shim for SoftwareBitmap). */
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Globalization;

namespace {

/* Join the WinRT apartment for this thread; tolerate a thread that is already
 * in a different apartment (OCR works from either). */
void ensure_apartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        if (e.code() != RPC_E_CHANGED_MODE) throw;
    }
}

/* The user's configured languages first, then English as a fallback.
 * Returns a null engine when no OCR language pack is installed. */
OcrEngine try_make_engine() {
    OcrEngine engine{ nullptr };
    try { engine = OcrEngine::TryCreateFromUserProfileLanguages(); } catch (...) {}
    if (!engine) {
        try { engine = OcrEngine::TryCreateFromLanguage(Language(L"en-US")); } catch (...) {}
    }
    return engine;
}

void hint_once() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        std::fprintf(stderr,
            "LISA: Windows OCR is unavailable (no OCR language pack installed).\n"
            "      Scanned PDFs will have no text layer. To enable OCR:\n"
            "        Settings > Time & Language > Language & region >\n"
            "        English > Language options > add\n"
            "        \"Optical character recognition\".\n"
            "      On N/KN Windows editions, install the Media Feature Pack first.\n");
    });
}

} /* namespace */

extern "C" int lisa_ocr_available(void) {
    try {
        ensure_apartment();
        if (try_make_engine()) return 1;
    } catch (...) {}
    hint_once();
    return 0;
}

extern "C" int lisa_ocr_image(const unsigned char* pixels, int width, int height,
                              int stride, char** out) {
    if (out) *out = nullptr;
    if (pixels == nullptr || out == nullptr || width <= 0 || height <= 0 ||
        stride < width * 4)
        return LISA_PLAT_EINVAL;

    try {
        ensure_apartment();
        OcrEngine engine = try_make_engine();
        if (!engine) { hint_once(); return LISA_PLAT_OK; }  /* no text, not an error */

        /* Copy the caller's BGRx rows into a Bgra8 SoftwareBitmap (alpha
         * ignored), honouring both strides. */
        SoftwareBitmap bmp(BitmapPixelFormat::Bgra8, width, height,
                           BitmapAlphaMode::Ignore);
        {
            BitmapBuffer buffer = bmp.LockBuffer(BitmapBufferAccessMode::Write);
            auto reference = buffer.CreateReference();
            uint8_t* dst = nullptr;
            uint32_t capacity = 0;
            auto access = reference.as<IMemoryBufferByteAccess>();
            check_hresult(access->GetBuffer(&dst, &capacity));
            BitmapPlaneDescription desc = buffer.GetPlaneDescription(0);
            for (int y = 0; y < height; ++y) {
                std::memcpy(dst + desc.StartIndex + (size_t)y * (size_t)desc.Stride,
                            pixels + (size_t)y * (size_t)stride,
                            (size_t)width * 4);
            }
        }

        OcrResult result = engine.RecognizeAsync(bmp).get();

        std::string text;
        for (auto const& line : result.Lines()) {
            text += winrt::to_string(line.Text());
            text += '\n';
        }

        char* buf = (char*)std::malloc(text.size() + 1);
        if (buf == nullptr) return LISA_PLAT_ENOMEM;
        std::memcpy(buf, text.c_str(), text.size() + 1);
        *out = buf;
        return LISA_PLAT_OK;
    } catch (std::bad_alloc const&) {
        return LISA_PLAT_ENOMEM;
    } catch (winrt::hresult_error const&) {
        return LISA_PLAT_EIO;
    } catch (...) {
        return LISA_PLAT_EIO;
    }
}
