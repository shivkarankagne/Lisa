/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * macOS desktop services for the platform layer (platform.h, "desktop"):
 * the folder dialog and file drops on the GUI's web view. Built only on
 * Apple platforms; manual retain/release (no ARC needed: nothing here
 * keeps objects past a call except the drop state, held by the class).
 */

#include "platform.h"

#import <Cocoa/Cocoa.h>
#import <Vision/Vision.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

char* lisa_choose_folder(void) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = YES;
        panel.allowsMultipleSelection = NO;
        panel.prompt = @"Add";
        panel.message = @"Choose a folder or file to add to LISA";
        if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return NULL;
        const char* p = panel.URLs[0].fileSystemRepresentation;
        return p ? strdup(p) : NULL;
    }
}

/* ---- file drops ----------------------------------------------------------
 * The web view's class gets replacement drag methods (the originals are
 * kept and called for everything else): for our view, a drop of Finder
 * files or folders passes their paths to the callback. A page cannot learn
 * the path of a dropped folder, so this is done natively. (A runtime
 * subclass of WKWebView trips an AppKit assertion when the window closes,
 * so the methods are replaced in place instead.) */

static id g_drop_view;
static lisa_drop_fn g_drop_fn;
static void* g_drop_user;
static IMP g_orig_entered, g_orig_updated, g_orig_perform;

static NSArray<NSURL*>* file_urls(id<NSDraggingInfo> info) {
    NSDictionary* opts = @{ NSPasteboardURLReadingFileURLsOnlyKey: @YES };
    return [[info draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ] options:opts];
}

typedef NSDragOperation (*drag_op_fn)(id, SEL, id);
typedef BOOL (*drag_perform_fn)(id, SEL, id);

static NSDragOperation lisa_dragging_entered(id self, SEL sel, id<NSDraggingInfo> info) {
    if (self == g_drop_view && file_urls(info).count > 0) return NSDragOperationCopy;
    return g_orig_entered ? ((drag_op_fn)g_orig_entered)(self, sel, info) : NSDragOperationNone;
}

static NSDragOperation lisa_dragging_updated(id self, SEL sel, id<NSDraggingInfo> info) {
    if (self == g_drop_view && file_urls(info).count > 0) return NSDragOperationCopy;
    return g_orig_updated ? ((drag_op_fn)g_orig_updated)(self, sel, info) : NSDragOperationNone;
}

static BOOL lisa_perform_drag(id self, SEL sel, id<NSDraggingInfo> info) {
    NSArray<NSURL*>* urls = self == g_drop_view ? file_urls(info) : nil;
    if (urls.count == 0) return g_orig_perform ? ((drag_perform_fn)g_orig_perform)(self, sel, info) : NO;
    int n = (int)urls.count;
    const char** paths = (const char**)calloc((size_t)n, sizeof(char*));
    if (paths == NULL) return NO;
    int k = 0;
    for (NSURL* u in urls) {
        const char* p = u.fileSystemRepresentation;
        if (p) paths[k++] = p;
    }
    if (g_drop_fn && k > 0) g_drop_fn(g_drop_user, paths, k);
    free(paths);
    return YES;
}

/* Replace sel on cls (adding an override if cls only inherits it); return the original. */
static IMP replace(Class cls, SEL sel, IMP imp) {
    Method m = class_getInstanceMethod(cls, sel);
    if (m == NULL) return NULL;
    IMP orig = method_getImplementation(m);
    if (!class_addMethod(cls, sel, imp, method_getTypeEncoding(m))) method_setImplementation(m, imp);
    return orig;
}

int lisa_file_drops_install(void* native_view, lisa_drop_fn fn, void* user) {
    if (native_view == NULL || fn == NULL) return LISA_PLAT_EINVAL;
    id view = (id)native_view;
    static int installed;
    if (!installed) {
        Class cls = [view class];
        g_orig_entered = replace(cls, @selector(draggingEntered:), (IMP)lisa_dragging_entered);
        g_orig_updated = replace(cls, @selector(draggingUpdated:), (IMP)lisa_dragging_updated);
        g_orig_perform = replace(cls, @selector(performDragOperation:), (IMP)lisa_perform_drag);
        installed = 1;
    }
    g_drop_view = view;
    g_drop_fn = fn;
    g_drop_user = user;
    [(NSView*)view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return LISA_PLAT_OK;
}

/* ---- OCR (Apple Vision) --------------------------------------------------- */

int lisa_ocr_available(void) {
    return 1;
}

int lisa_ocr_image(const unsigned char* pixels, int width, int height, int stride, char** out) {
    if (out) *out = NULL;
    if (pixels == NULL || out == NULL || width <= 0 || height <= 0 || stride < width * 4) return LISA_PLAT_EINVAL;
    int rc = LISA_PLAT_EIO;
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, pixels, (size_t)stride * (size_t)height, NULL);
        CGImageRef img = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)stride, cs,
                                       kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, dp, NULL,
                                       false, kCGRenderingIntentDefault);
        if (img) {
            VNRecognizeTextRequest* req = [[VNRecognizeTextRequest alloc] init];
            req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            req.usesLanguageCorrection = YES;
            VNImageRequestHandler* h = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
            NSError* err = nil;
            if ([h performRequests:@[ req ] error:&err]) {
                NSMutableString* text = [NSMutableString string];
                for (VNRecognizedTextObservation* o in req.results) {
                    VNRecognizedText* best = [[o topCandidates:1] firstObject];
                    if (best == nil) continue;
                    [text appendString:best.string];
                    [text appendString:@"\n"];
                }
                const char* u = text.UTF8String;
                *out = strdup(u ? u : "");
                rc = *out ? LISA_PLAT_OK : LISA_PLAT_ENOMEM;
            }
            [h release];
            [req release];
            CGImageRelease(img);
        }
        CGDataProviderRelease(dp);
        CGColorSpaceRelease(cs);
    }
    return rc;
}
