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
project(lisa VERSION 0.1.0 LANGUAGES C ASM)

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

add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
)

if(LISA_SANITIZE)
    add_compile_options(
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- platform / architecture -------------------------------------------

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    message(FATAL_ERROR
        "LISA 1.0 targets ARM64 only (plan decision 10). "
        "Detected: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ---- core library ------------------------------------------------------

add_library(lisa_core STATIC
    src/retrieval/retrieval_scalar.c
    src/storage/storage.c
    src/kernels/arm64/lisa_asm_wrapper.c
    src/kernels/arm64/lisa_ultra_mac.s
)
target_include_directories(lisa_core PUBLIC src)
target_link_libraries(lisa_core PUBLIC m)

# ---- executable --------------------------------------------------------

add_executable(lisa
    src/cli/main.c
    src/api/http.c
)
target_link_libraries(lisa PRIVATE lisa_core)

# ---- tests -------------------------------------------------------------

if(LISA_BUILD_TESTS)
    enable_testing()

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
