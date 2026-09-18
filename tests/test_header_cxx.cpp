// SPDX-License-Identifier: Apache-2.0
//
// test_header_cxx — include/lisa.h compiles and links as C++ (extern "C",
// no C-only constructs), so C++ programs and SDK bindings can use it.

#include "lisa.h"

#include <cstdio>
#include <cstring>

int main() {
    int major = -1;
    const char* v = lisa_version(&major, nullptr, nullptr);
    lisa_context_config_t cfg = LISA_CONTEXT_CONFIG_INIT;
    lisa_context_t* ctx = nullptr;
    if (lisa_context_create(&cfg, &ctx) != LISA_OK || ctx == nullptr) return 1;
    lisa_context_destroy(ctx);
    lisa_search_options_t o = LISA_SEARCH_OPTIONS_INIT;
    lisa_collection_info_t info = LISA_COLLECTION_INFO_INIT;
    if (o.top_k != 5 || info.struct_size != sizeof(info)) return 1;
    if (major != LISA_VERSION_MAJOR || std::strcmp(v, LISA_VERSION_STRING) != 0) return 1;
    std::printf("PASS: lisa.h usable from C++ (%s)\n", v);
    return 0;
}
