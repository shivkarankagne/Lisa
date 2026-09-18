# SPDX-License-Identifier: Apache-2.0
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
project(lisa VERSION 0.1.0 LANGUAGES C CXX ASM)

option(LISA_SANITIZE         "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LISA_BUILD_TESTS      "Build tests"      ON)
option(LISA_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(LISA_BUILD_LLAMA      "Build vendored llama.cpp (model runtime, W4)" ON)

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
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- llama.cpp (third_party/llama.cpp/INTAKE.md) ------------------------
# Added before LISA's warning flags, which must not apply to it.

if(LISA_BUILD_LLAMA)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    foreach(opt LLAMA_BUILD_COMMON LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS
                LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER LLAMA_BUILD_APP
                LLAMA_BUILD_UI GGML_NATIVE GGML_OPENMP GGML_CCACHE)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(GGML_METAL               ON CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
endif()

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

# ---- core library ------------------------------------------------------

add_library(lisa_core STATIC
    src/platform/platform_posix.c
    src/kernels/registry.c
    src/kernels/kernels_scalar.c
    src/kernels/arm64/l2_neon.c
    src/retrieval/retrieval_scalar.c
    src/retrieval/retrieval_search.c
    src/storage/storage.c
    src/storage/store.c
    src/kernels/arm64/lisa_asm_wrapper.c
    src/kernels/arm64/lisa_ultra_mac.s
)
target_include_directories(lisa_core PUBLIC src)
target_link_libraries(lisa_core PUBLIC lisa_sqlite m)

# ---- executable --------------------------------------------------------

add_executable(lisa
    src/cli/main.c
    src/api/http.c
)
target_link_libraries(lisa PRIVATE lisa_core)

# ---- tests -------------------------------------------------------------

if(LISA_BUILD_TESTS)
    enable_testing()

    add_library(lisa_unity STATIC third_party/unity/unity.c)
    target_include_directories(lisa_unity PUBLIC third_party/unity)
    target_compile_options(lisa_unity PRIVATE -w)

    function(lisa_c_test name)
        add_executable(${name} tests/${name}.c ${ARGN})
        target_link_libraries(${name} PRIVATE lisa_core)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    lisa_c_test(test_retrieval)
    lisa_c_test(test_retrieval_asm)
    lisa_c_test(test_edge_cases)
    lisa_c_test(test_storage)
    lisa_c_test(test_storage_mutation)
    lisa_c_test(test_kernel_abi tests/kernel_abi_probe.s)
    lisa_c_test(test_kernels)
    add_executable(test_platform tests/test_platform.c)
    target_link_libraries(test_platform PRIVATE lisa_core)
    add_test(NAME test_platform COMMAND test_platform ${CMAKE_BINARY_DIR}/scratch)

    add_executable(test_store tests/test_store.c)
    target_link_libraries(test_store PRIVATE lisa_core lisa_unity)
    add_test(NAME test_store COMMAND test_store ${CMAKE_BINARY_DIR}/scratch)
    add_executable(test_store_crash tests/test_store_crash.c)
    target_link_libraries(test_store_crash PRIVATE lisa_core)
    add_test(NAME test_store_crash COMMAND test_store_crash ${CMAKE_BINARY_DIR}/scratch 40)

    add_executable(test_sqlite tests/test_sqlite.c)
    target_link_libraries(test_sqlite PRIVATE lisa_core lisa_sqlite lisa_unity)
    add_test(NAME test_sqlite COMMAND test_sqlite ${CMAKE_BINARY_DIR}/scratch)

    if(LISA_BUILD_LLAMA)
        add_executable(test_llama_link tests/test_llama_link.c)
        target_link_libraries(test_llama_link PRIVATE llama lisa_unity)
        add_test(NAME test_llama_link COMMAND test_llama_link)
    endif()
    lisa_c_test(test_search)
    add_test(NAME test_search_scalar COMMAND test_search)
    set_tests_properties(test_search_scalar PROPERTIES ENVIRONMENT "LISA_KERNEL=scalar")

    add_executable(fuzz_retrieval_asm tests/fuzz_retrieval_asm.c)
    target_link_libraries(fuzz_retrieval_asm PRIVATE lisa_core)
    add_test(NAME fuzz_retrieval_asm COMMAND fuzz_retrieval_asm 1000 12345)

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
        FAIL_REGULAR_EXPRESSION "SKIP"
        ENVIRONMENT
            "LISA_BIN=$<TARGET_FILE:lisa>;LISA_TEST_INDEX=${LISA_TESTDATA}/vectors_768.bin;LISA_TEST_QUERY=${LISA_TESTDATA}/query_768.txt;LISA_BUILD_COLLECTION=$<TARGET_FILE:build_collection>")

    add_test(NAME test_api COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_api.sh)
    set_tests_properties(test_api PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT "LISA_BIN=$<TARGET_FILE:lisa>")
endif()

# ---- benchmarks (off by default; performance work is plan L1) ------------

if(LISA_BUILD_BENCHMARKS)
    add_executable(bench_retrieval benchmark/bench_retrieval.c)
    target_link_libraries(bench_retrieval PRIVATE lisa_core)
    add_executable(bench_compare benchmark/bench_compare.c)
    target_link_libraries(bench_compare PRIVATE lisa_core)
endif()
