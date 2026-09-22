/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * platform_ocr_none.c — OCR stub for platforms without a system text
 * recogniser (everything except macOS, which uses Apple Vision in
 * platform_macos.m).
 *
 * With no recogniser, a scanned page is treated exactly like a PDF that
 * has no text layer: the caller reports "no text" for that page rather
 * than failing. A recogniser (e.g. Tesseract) can replace this file
 * later without touching any caller.
 */

#include "platform.h"

int lisa_ocr_available(void) {
    return 0;
}

int lisa_ocr_image(const unsigned char* pixels, int width, int height, int stride, char** out) {
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    if (out) *out = NULL;
    return LISA_PLAT_EINVAL;
}
