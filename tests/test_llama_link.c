/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_llama_link — the vendored llama.cpp builds as static libraries,
 * links into a C program, and initialises with the expected backends.
 *
 * Only the CPU backend is required: CI machines may have no usable GPU.
 * Model loading and generation are tested in W4.
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "llama.h"

void setUp(void) {}
void tearDown(void) {}

static void test_cpu_backend_present(void) {
    int found_cpu = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t d = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_CPU) found_cpu = 1;
    }
    TEST_ASSERT_TRUE(found_cpu);
}

static void test_build_features(void) {
    const char* info = llama_print_system_info();
    TEST_ASSERT_NOT_NULL(info);
    /* NEON is mandatory on ARM64; Metal library must be embedded. */
    TEST_ASSERT_NOT_NULL(strstr(info, "NEON = 1"));
    TEST_ASSERT_NOT_NULL(strstr(info, "EMBED_LIBRARY = 1"));
}

int main(void) {
    llama_backend_init();
    UNITY_BEGIN();
    RUN_TEST(test_cpu_backend_present);
    RUN_TEST(test_build_features);
    int failures = UNITY_END();
    llama_backend_free();
    return failures == 0 ? 0 : 1;
}
