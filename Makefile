# LISA — single-binary build target
#
# Produces one native executable: build/lisa
#
# Flags note:
#   -O0 is used deliberately. On the current ARM64 macOS toolchain,
#   -O2 was observed to miscompile comparison logic in test code.
#   This is documented in LISA_REPORT_AND_UPDATE_001.md.
#   Do not change these flags without a separate, measured package.

CC      := gcc
CFLAGS  := -O0 -g
LDFLAGS := -lm

SRC_DIR   := src
BUILD_DIR := build

SOURCES := \
    $(SRC_DIR)/cli/main.c \
    $(SRC_DIR)/retrieval/retrieval_scalar.c \
    $(SRC_DIR)/storage/storage.c \
    $(SRC_DIR)/api/http.c \
    $(SRC_DIR)/kernels/arm64/lisa_asm_wrapper.c \
    $(SRC_DIR)/kernels/arm64/lisa_ultra_mac.s

TARGET := $(BUILD_DIR)/lisa

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./tests/test_retrieval
	./tests/test_retrieval_asm
	./tests/test_edge_cases
	./tests/test_storage
	./tests/test_cli.sh
