# SPDX-License-Identifier: BUSL-1.1
#
# LISA — single-binary build.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Sanitizer build (ASan + UBSan), used for every test run before commit:
#
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLISA_SANITIZE=ON
#   cmake --build build-asan
#   ctest --test-dir build-asan --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(lisa VERSION 0.6.0 LANGUAGES C CXX ASM OBJC)

option(LISA_SANITIZE         "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LISA_BUILD_TESTS      "Build tests"      ON)
option(LISA_BUILD_BENCHMARKS "Build benchmarks" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Product optimisation level is -O2 (plan W1). Asserts stay enabled.
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")

# Sanitizers apply to everything, including third-party code.
if(LISA_SANITIZE)
    add_compile_options(
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -fsanitize-ignorelist=${CMAKE_SOURCE_DIR}/third_party/sanitizer-ignorelist.txt
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- llama.cpp (third_party/llama.cpp/INTAKE.md) ------------------------
# Added before LISA's warning flags, which must not apply to it.

# Required: LISA's model runtime (W4).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(opt LLAMA_BUILD_COMMON LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS
            LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER LLAMA_BUILD_APP
            LLAMA_BUILD_UI GGML_NATIVE GGML_OPENMP GGML_CCACHE)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
# Metal is Apple's GPU backend. On other platforms llama.cpp falls back
# to its portable CPU backend, which is all a headless Linux build needs
# for now; a GPU backend (CUDA, Vulkan) is a later, separate choice.
if(APPLE)
    set(GGML_METAL               ON CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

# LISA's own code: warnings on.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
)

# ---- platform / architecture -------------------------------------------

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    message(FATAL_ERROR
        "LISA 1.0 targets ARM64 only (plan decision 10). "
        "Detected: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ---- third-party (see third_party/README.md) ---------------------------

add_library(lisa_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(lisa_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(lisa_sqlite PUBLIC
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_compile_options(lisa_sqlite PRIVATE -w)

# ---- version: include/lisa.h must match project(VERSION) ---------------

file(STRINGS include/lisa.h LISA_HEADER_VERSION REGEX "^#define LISA_VERSION_STRING ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" LISA_HEADER_VERSION "${LISA_HEADER_VERSION}")
if(NOT LISA_HEADER_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "include/lisa.h version ${LISA_HEADER_VERSION} != project version ${PROJECT_VERSION}")
endif()

add_library(lisa_md4c STATIC third_party/md4c/md4c.c)
target_include_directories(lisa_md4c PUBLIC third_party/md4c)
target_compile_options(lisa_md4c PRIVATE -w)

# miniz (third_party/miniz/INTAKE.md): reads .docx (zip) files.
add_library(lisa_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(lisa_miniz PUBLIC third_party/miniz)
target_compile_definitions(lisa_miniz PUBLIC MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(lisa_miniz PRIVATE -w)

add_library(lisa_yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(lisa_yyjson PUBLIC third_party/yyjson)
target_compile_options(lisa_yyjson PRIVATE -w)

# CivetWeb (third_party/civetweb/INTAKE.md): local HTTP only; no TLS, CGI,
# file serving, or caching.
add_library(lisa_civetweb STATIC third_party/civetweb/src/civetweb.c)
target_include_directories(lisa_civetweb PUBLIC third_party/civetweb/include)
target_compile_definitions(lisa_civetweb PUBLIC NO_SSL NO_CGI NO_FILES NO_CACHING)
target_compile_options(lisa_civetweb PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(lisa_civetweb PUBLIC Threads::Threads)

# webview (third_party/webview/INTAKE.md): native window for `lisa gui`.
if(APPLE)
    add_library(lisa_webview STATIC third_party/webview/src/webview.cc)
    target_include_directories(lisa_webview PUBLIC third_party/webview/include)
    target_compile_definitions(lisa_webview PUBLIC WEBVIEW_STATIC)
    target_compile_features(lisa_webview PRIVATE cxx_std_11)
    target_compile_options(lisa_webview PRIVATE -w)
    target_link_libraries(lisa_webview PUBLIC "-framework WebKit" "-framework Cocoa")
endif()

add_library(lisa_utf8proc STATIC third_party/utf8proc/utf8proc.c)
target_include_directories(lisa_utf8proc PUBLIC third_party/utf8proc)
target_compile_definitions(lisa_utf8proc PUBLIC UTF8PROC_STATIC)
target_compile_options(lisa_utf8proc PRIVATE -w)

# PDFium: built from source by scripts/build_pdfium.sh (no static library
# is published for macOS). Required for PDF support.
set(LISA_PDFIUM_DIR "${CMAKE_SOURCE_DIR}/.deps/pdfium" CACHE PATH "Static PDFium build")
option(LISA_REQUIRE_PDF "Fail if PDFium is not available" ON)
if(EXISTS "${LISA_PDFIUM_DIR}/lib/libpdfium.a")
    set(LISA_HAVE_PDFIUM ON)
    add_library(lisa_pdfium STATIC IMPORTED)
    set_target_properties(lisa_pdfium PROPERTIES
        IMPORTED_LOCATION "${LISA_PDFIUM_DIR}/lib/libpdfium.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LISA_PDFIUM_DIR}/include")
    if(APPLE)
        target_link_libraries(lisa_pdfium INTERFACE
            "-framework AppKit" "-framework CoreFoundation" "-framework CoreGraphics"
            "-framework Foundation" "-framework Security")
    endif()
elseif(LISA_REQUIRE_PDF)
    message(FATAL_ERROR
        "PDFium not found in ${LISA_PDFIUM_DIR}. Get it with scripts/fetch_pdfium.sh "
        "(prebuilt, verified) or build it with scripts/build_pdfium.sh "
        "(or configure with -DLISA_REQUIRE_PDF=OFF to build without PDF support).")
else()
    message(WARNING "Building WITHOUT PDF support (LISA_REQUIRE_PDF=OFF).")
endif()

# ---- core library ------------------------------------------------------

add_library(lisa_core STATIC
    src/api/lisa_api.c
    src/api/lisa_models.c
    src/api/lisa_ingest.c
    src/models/models.c
    src/documents/documents.c
    src/documents/markdown.c
    src/documents/chunk.c
    src/documents/docx.c
    src/ingest/ingest.c
    src/platform/platform_posix.c
    src/kernels/registry.c
    src/kernels/kernels_scalar.c
    src/retrieval/retrieval_scalar.c
    src/retrieval/retrieval_search.c
    src/retrieval/fusion.c
    src/retrieval/index.c
    src/api/lisa_search.c
    src/api/lisa_ask.c
    src/context/context.c
    src/storage/storage.c
    src/storage/store.c
)

# NEON is ARM-only; its source is self-guarded but need not be fed to a
# non-ARM compiler at all. The assembly kernel is Apple-ARM64 only and is
# a test-only path (plan D2), so it is excluded everywhere else.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    target_sources(lisa_core PRIVATE src/kernels/arm64/l2_neon.c)
    if(APPLE)
        target_sources(lisa_core PRIVATE
            src/kernels/arm64/lisa_asm_wrapper.c
            src/kernels/arm64/lisa_ultra_mac.s)
        set(LISA_HAVE_ASM_KERNEL ON)
    endif()
endif()
# Where the assembly kernel is absent, its test-only entry point is
# provided by a scalar stand-in so the differential tests still link.
if(NOT LISA_HAVE_ASM_KERNEL)
    target_sources(lisa_core PRIVATE src/kernels/search_asm_portable.c)
endif()
target_include_directories(lisa_core PUBLIC include src)
target_link_libraries(lisa_core PUBLIC lisa_sqlite m PRIVATE llama vendor::hash lisa_md4c lisa_utf8proc lisa_miniz)
if(APPLE)
    # macOS: the folder picker, drag-drop, and Apple Vision OCR.
    target_sources(lisa_core PRIVATE src/platform/platform_macos.m)
    set_source_files_properties(src/platform/platform_macos.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
    target_link_libraries(lisa_core PUBLIC "-framework Cocoa" "-framework Vision")
else()
    # No system recogniser: scanned pages are reported as having no text.
    target_sources(lisa_core PRIVATE src/platform/platform_ocr_none.c)
endif()
if(LISA_HAVE_PDFIUM)
    target_sources(lisa_core PRIVATE src/documents/pdf.c)
    target_compile_definitions(lisa_core PRIVATE LISA_HAVE_PDFIUM)
    target_link_libraries(lisa_core PRIVATE lisa_pdfium)
endif()

# ---- executable --------------------------------------------------------

# The program layer (data dir, config, models) and the HTTP server; used by
# the CLI and by tests.
add_library(lisa_app STATIC
    src/app/app.c
    src/app/app_json.c
    src/app/log.c
    src/http/server.c
)
target_link_libraries(lisa_app PUBLIC lisa_core lisa_yyjson lisa_civetweb)

# The GUI (gui/): its files are compiled into the binary, and on macOS
# `lisa gui` shows them in a native window (webview).
set(LISA_GUI_FILES ${CMAKE_SOURCE_DIR}/gui/index.html ${CMAKE_SOURCE_DIR}/gui/app.css ${CMAKE_SOURCE_DIR}/gui/app.js)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/gui_assets.c
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/gui_assets.c -DSRC=${CMAKE_SOURCE_DIR}/gui
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${LISA_GUI_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding GUI files"
    VERBATIM)
add_library(lisa_gui STATIC ${CMAKE_BINARY_DIR}/gui_assets.c src/gui/window.c)
target_link_libraries(lisa_gui PUBLIC lisa_app)
if(APPLE)
    target_link_libraries(lisa_gui PUBLIC lisa_webview)
    target_compile_definitions(lisa_gui PRIVATE LISA_HAVE_WEBVIEW)
endif()

add_executable(lisa src/cli/main.c)
target_link_libraries(lisa PRIVATE lisa_gui)

# ---- tests -------------------------------------------------------------

if(LISA_BUILD_TESTS)
    enable_testing()
    set(LISA_MODELS_DIR "${CMAKE_SOURCE_DIR}/models" CACHE PATH "Directory with the default model files")

    add_library(lisa_unity STATIC third_party/unity/unity.c)
    target_include_directories(lisa_unity PUBLIC third_party/unity)
    target_compile_options(lisa_unity PRIVATE -w)

    function(lisa_c_test name)
        add_executable(${name} tests/${name}.c ${ARGN})
        target_link_libraries(${name} PRIVATE lisa_core)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    lisa_c_test(test_retrieval)
    lisa_c_test(test_edge_cases)
    lisa_c_test(test_storage)
    lisa_c_test(test_storage_mutation)
    lisa_c_test(test_kernels)
    # These exercise the assembly kernel itself, so they run only where it
    # is built (Apple ARM64). Elsewhere the differential tests above use
    # the scalar stand-in for lisa_search_exact_asm.
    if(LISA_HAVE_ASM_KERNEL)
        lisa_c_test(test_retrieval_asm)
        lisa_c_test(test_kernel_abi tests/kernel_abi_probe.s)
    endif()
    add_executable(test_platform tests/test_platform.c)
    target_link_libraries(test_platform PRIVATE lisa_core)
    add_test(NAME test_platform COMMAND test_platform ${CMAKE_BINARY_DIR}/scratch)

    add_executable(test_store tests/test_store.c)
    target_link_libraries(test_store PRIVATE lisa_core lisa_unity)
    add_test(NAME test_store COMMAND test_store ${CMAKE_BINARY_DIR}/scratch)
    add_executable(test_store_crash tests/test_store_crash.c)
    target_link_libraries(test_store_crash PRIVATE lisa_core)
    add_test(NAME test_store_crash COMMAND test_store_crash ${CMAKE_BINARY_DIR}/scratch 40)

    add_executable(test_public_api tests/test_public_api.c)
    target_link_libraries(test_public_api PRIVATE lisa_core lisa_unity)
    add_test(NAME test_public_api COMMAND test_public_api ${CMAKE_BINARY_DIR}/scratch)

    add_executable(test_header_cxx tests/test_header_cxx.cpp)
    target_link_libraries(test_header_cxx PRIVATE lisa_core)
    target_compile_options(test_header_cxx PRIVATE -Wall -Wextra -pedantic)
    add_test(NAME test_header_cxx COMMAND test_header_cxx)

    add_executable(test_documents tests/test_documents.c)
    target_link_libraries(test_documents PRIVATE lisa_core lisa_unity)
    if(LISA_HAVE_PDFIUM)
        target_compile_definitions(test_documents PRIVATE LISA_HAVE_PDFIUM)
    endif()
    add_test(NAME test_documents
             COMMAND test_documents ${CMAKE_BINARY_DIR}/scratch ${CMAKE_SOURCE_DIR}/tests/fixtures)

    add_executable(test_ingest tests/test_ingest.c)
    target_link_libraries(test_ingest PRIVATE lisa_core lisa_unity)
    add_test(NAME test_ingest
             COMMAND test_ingest ${CMAKE_BINARY_DIR}/scratch ${CMAKE_SOURCE_DIR}/tests/fixtures ${LISA_MODELS_DIR})
    set_tests_properties(test_ingest PROPERTIES TIMEOUT 1800)

    add_executable(test_ingest_api tests/test_ingest_api.c)
    target_link_libraries(test_ingest_api PRIVATE lisa_core lisa_unity)
    add_test(NAME test_ingest_api
             COMMAND test_ingest_api ${CMAKE_BINARY_DIR}/scratch ${CMAKE_SOURCE_DIR}/tests/fixtures ${LISA_MODELS_DIR})
    set_tests_properties(test_ingest_api PROPERTIES TIMEOUT 1800)

    add_executable(test_query tests/test_query.c)
    target_link_libraries(test_query PRIVATE lisa_core lisa_unity)
    add_test(NAME test_query COMMAND test_query ${CMAKE_BINARY_DIR}/scratch ${LISA_MODELS_DIR})
    set_tests_properties(test_query PROPERTIES TIMEOUT 1800)

    add_executable(test_context tests/test_context.c)
    target_link_libraries(test_context PRIVATE lisa_core lisa_unity)
    add_test(NAME test_context COMMAND test_context)

    add_executable(test_ask tests/test_ask.c)
    target_link_libraries(test_ask PRIVATE lisa_core lisa_unity)
    add_test(NAME test_ask
             COMMAND test_ask ${CMAKE_BINARY_DIR}/scratch ${CMAKE_SOURCE_DIR}/tests/fixtures ${LISA_MODELS_DIR})
    set_tests_properties(test_ask PROPERTIES TIMEOUT 1800)
    if(LISA_SANITIZE)
        # False positive in llama.cpp, not LISA: after llama_batch_allocr::clear(),
        # vector<int>::resize(1) value-initialises element 0 with bzero before
        # Apple libc++ (clang 21) updates its container annotation, and ASan
        # reports "container-overflow" at byte 0 of the vector's own buffer.
        # Only this check is off, only here; all other ASan/UBSan checks run.
        set_tests_properties(test_ask PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_container_overflow=0")
    endif()

    add_executable(test_http tests/test_http.c)
    target_link_libraries(test_http PRIVATE lisa_app lisa_unity)
    add_test(NAME test_http
             COMMAND test_http ${CMAKE_BINARY_DIR}/scratch ${CMAKE_SOURCE_DIR}/tests/fixtures ${LISA_MODELS_DIR})
    set_tests_properties(test_http PROPERTIES TIMEOUT 1800)
    if(LISA_SANITIZE)
        # Same llama.cpp false positive as test_ask (see there).
        set_tests_properties(test_http PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_container_overflow=0")
    endif()

    add_executable(test_sqlite tests/test_sqlite.c)
    target_link_libraries(test_sqlite PRIVATE lisa_core lisa_sqlite lisa_unity)
    add_test(NAME test_sqlite COMMAND test_sqlite ${CMAKE_BINARY_DIR}/scratch)

    add_executable(test_models tests/test_models.c)
    target_link_libraries(test_models PRIVATE lisa_core llama lisa_unity)
    add_test(NAME test_models COMMAND test_models ${CMAKE_BINARY_DIR}/scratch ${LISA_MODELS_DIR})
    set_tests_properties(test_models PROPERTIES TIMEOUT 1800)

    add_executable(test_llama_link tests/test_llama_link.c)
    target_link_libraries(test_llama_link PRIVATE llama lisa_unity)
    add_test(NAME test_llama_link COMMAND test_llama_link)
    lisa_c_test(test_search)
    add_test(NAME test_search_scalar COMMAND test_search)
    set_tests_properties(test_search_scalar PROPERTIES ENVIRONMENT "LISA_KERNEL=scalar")

    if(LISA_HAVE_ASM_KERNEL)
        add_executable(fuzz_retrieval_asm tests/fuzz_retrieval_asm.c)
        target_link_libraries(fuzz_retrieval_asm PRIVATE lisa_core)
        add_test(NAME fuzz_retrieval_asm COMMAND fuzz_retrieval_asm 1000 12345)
    endif()

    # Helpers for the end-to-end shell tests.
    add_executable(gen_test_data tests/gen_test_data.c)
    add_executable(build_collection tests/build_collection.c)
    target_link_libraries(build_collection PRIVATE lisa_core)

    set(LISA_TESTDATA ${CMAKE_BINARY_DIR}/testdata)
    file(MAKE_DIRECTORY ${LISA_TESTDATA})

    add_test(NAME gen_test_data
        COMMAND gen_test_data
            ${LISA_TESTDATA}/vectors_768.bin
            ${LISA_TESTDATA}/query_768.txt
            10000 768 42)
    set_tests_properties(gen_test_data PROPERTIES FIXTURES_SETUP testdata)

    add_test(NAME test_cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_cli.sh)
    set_tests_properties(test_cli PROPERTIES
        FIXTURES_REQUIRED testdata
        TIMEOUT 1800
        ENVIRONMENT
            "LISA_BIN=$<TARGET_FILE:lisa>;LISA_TEST_INDEX=${LISA_TESTDATA}/vectors_768.bin;LISA_BUILD_COLLECTION=$<TARGET_FILE:build_collection>;LISA_MODELS=${LISA_MODELS_DIR};LISA_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures")
    if(LISA_SANITIZE)
        # Same llama.cpp false positive as test_ask (see there).
        set_tests_properties(test_cli PROPERTIES ENVIRONMENT
            "LISA_BIN=$<TARGET_FILE:lisa>;LISA_TEST_INDEX=${LISA_TESTDATA}/vectors_768.bin;LISA_BUILD_COLLECTION=$<TARGET_FILE:build_collection>;LISA_MODELS=${LISA_MODELS_DIR};LISA_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures;ASAN_OPTIONS=detect_container_overflow=0")
    endif()

    # The GUI in headless Chrome; skipped (77) where Node or Chrome is missing.
    add_test(NAME test_gui COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_gui.sh)
    set_tests_properties(test_gui PROPERTIES
        TIMEOUT 1800
        SKIP_RETURN_CODE 77
        ENVIRONMENT "LISA_BIN=$<TARGET_FILE:lisa>;LISA_MODELS=${LISA_MODELS_DIR};LISA_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures")
    if(LISA_SANITIZE)
        set_tests_properties(test_gui PROPERTIES ENVIRONMENT
            "LISA_BIN=$<TARGET_FILE:lisa>;LISA_MODELS=${LISA_MODELS_DIR};LISA_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures;ASAN_OPTIONS=detect_container_overflow=0")
    endif()
endif()

# ---- benchmarks (off by default; performance work is plan L1) ------------

if(LISA_BUILD_BENCHMARKS)
    add_executable(bench_retrieval benchmark/bench_retrieval.c)
    target_link_libraries(bench_retrieval PRIVATE lisa_core)
    add_executable(bench_compare benchmark/bench_compare.c)
    target_link_libraries(bench_compare PRIVATE lisa_core)
    add_executable(bench_models benchmark/bench_models.c)
    target_link_libraries(bench_models PRIVATE lisa_core)
endif()
