# Kitty DOOM Makefile
# Requires GNU make 3.81+

# Set default goal before including other makefiles
.DEFAULT_GOAL := all

# Include build utilities and external download rules
include mk/common.mk
include mk/external.mk

# Directories and output
OUT := build
TARGET := $(OUT)/kitty-doom
TEST_DIR := tests
TEST_OUT := $(OUT)/tests

# Source files
SRCS := src/input.c src/main.c src/render.c src/base64.c src/sound.c

# Object files (placed in build directory)
OBJS := $(patsubst src/%.c,$(OUT)/%.o,$(SRCS))

# Dependency files
DEPS := $(OBJS:.o=.d)

# Compiler and flags
CC := cc
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g -Isrc -MMD -MP
LDLIBS := -lpthread

# Optional profiling mode (enable with: make PROFILE=1)
ifdef PROFILE
    CFLAGS += -DENABLE_PROFILING
endif

# NEON-specific flags (enabled on ARM/ARM64)
# The NEON implementation will only be active if __aarch64__ or __ARM_NEON is defined
NEON_FLAGS :=
ifeq ($(shell uname -m),aarch64)
    NEON_FLAGS := -march=armv8-a+simd
else ifeq ($(shell uname -m),arm64)
    NEON_FLAGS := -march=armv8-a+simd
endif

# SSE/SSSE3-specific flags (enabled on x86/x86_64)
SSE_FLAGS :=
ifeq ($(shell uname -m),x86_64)
    SSE_FLAGS := -mssse3
else ifeq ($(shell uname -m),i686)
    SSE_FLAGS := -mssse3
endif

ARCH_FLAGS = $(NEON_FLAGS) $(SSE_FLAGS)

# Check if compiler supports a specific flag
check_flag = $(shell $(CC) $(1) -E -xc /dev/null > /dev/null 2>&1 && echo $(1))

# Warning suppression candidates for PureDOOM (third-party code in main.c)
PUREDOOM_CFLAGS_TO_CHECK := \
    -Wno-parentheses \
    -Wno-enum-compare \
    -Wno-deprecated-non-prototype \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-unused-but-set-variable \
    -Wno-unused-but-set-parameter \
    -Wno-sign-compare \
    -Wno-missing-field-initializers \
    -Wno-unknown-pragmas \
    -Wno-sometimes-uninitialized \
    -Wno-unknown-warning-option \
    -Wno-string-concatenation \
    -Wno-enum-conversion \
    -Wno-implicit-fallthrough \
    -Wno-dangling-pointer \
    -Wno-maybe-uninitialized

# Only add flags that the compiler supports
PUREDOOM_CFLAGS := $(foreach flag,$(PUREDOOM_CFLAGS_TO_CHECK),$(call check_flag,$(flag)))

# Default target
.PHONY: all
all: $(TARGET) $(DOOM1_WAD) $(MINIAUDIO_HEADER) check-wad-symlink

# Check and create doom1.wad symlink if needed (for case-sensitive filesystems)
.PHONY: check-wad-symlink
check-wad-symlink: $(DOOM1_WAD)
	@if [ ! -e doom1.wad ] && [ -e $(DOOM1_WAD) ]; then \
		printf "  LINK\tdoom1.wad -> $(DOOM1_WAD)\n"; \
		ln -s $(DOOM1_WAD) doom1.wad; \
	fi

# Download game assets (DOOM1.WAD, PureDOOM.h, miniaudio.h)
.PHONY: download-assets
download-assets: $(DOOM1_WAD) $(PUREDOOM_HEADER) $(MINIAUDIO_HEADER)

# Run the game
.PHONY: run
run: $(TARGET) $(DOOM1_WAD) check-wad-symlink
	@$(TARGET)

# Run with profiling enabled (use: make profile)
.PHONY: profile
profile: clean
	@$(MAKE) PROFILE=1 all
	$(VECHO) "\nProfiling enabled. Run './$(TARGET)' to see performance stats.\n"

# Test targets
.PHONY: check
check: $(TEST_OUT)/bench-base64 $(TEST_OUT)/test-atomic-bitmap
	@echo ""
	@echo "========================================"
	@echo "  Running Test Suite"
	@echo "========================================"
	@echo ""
	@$(TEST_OUT)/bench-base64
	@echo ""
	@$(TEST_OUT)/test-atomic-bitmap
	@echo ""
	@chmod +x $(TEST_DIR)/perf-regression.sh
	@$(TEST_DIR)/perf-regression.sh

.PHONY: bench-zlib
bench-zlib: $(TEST_OUT)/bench-zlib
	$(VECHO) "Running zlib compression benchmark...\n"
	@$(TEST_OUT)/bench-zlib

# Build test binaries
$(TEST_OUT)/bench-base64: $(TEST_DIR)/bench-base64.c src/base64.c | $(TEST_OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) $(ARCH_FLAGS) -o $@ $^

$(TEST_OUT)/test-atomic-bitmap: $(TEST_DIR)/test-atomic-bitmap.c | $(TEST_OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(TEST_OUT)/bench-zlib: $(TEST_DIR)/bench-zlib.c | $(TEST_OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -o $@ $< -lz

$(TEST_OUT):
	$(Q)mkdir -p $(TEST_OUT)

# Link binary
$(TARGET): $(OBJS) | $(OUT)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDLIBS)

# Compile source files (depends on PureDOOM.h and miniaudio.h)
$(OUT)/%.o: src/%.c $(PUREDOOM_HEADER) $(MINIAUDIO_HEADER) | $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

# Special rule for main.c with PureDOOM warning suppression
$(OUT)/main.o: src/main.c $(PUREDOOM_HEADER) | $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) $(PUREDOOM_CFLAGS) -c -o $@ $<

# Special rule for base64.c with SIMD-enabled compilation
# This ensures arch/neon-base64.h and arch/sse-base64.h can use SIMD intrinsics
$(OUT)/base64.o: src/base64.c | $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) $(ARCH_FLAGS) -c -o $@ $<

# Special rule for sound.c with miniaudio.h dependency and relaxed warnings
# miniaudio.h is a large single-header library that may trigger some warnings
$(OUT)/sound.o: src/sound.c $(MINIAUDIO_HEADER) | $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

# Create build directory
$(OUT):
	$(Q)mkdir -p $(OUT)

# Clean build artifacts
.PHONY: clean
clean:
	$(VECHO) "  CLEAN\t\t$(OUT)\n"
	$(Q)rm -rf $(OUT)

# Clean everything including downloaded files
.PHONY: distclean
distclean: clean
	$(Q)$(MAKE) -s clean-external

# Include dependency files
-include $(DEPS)
