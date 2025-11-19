#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Performance regression test for kitty-doom
# Tracks core path performance: base64 encoding, atomic ops

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Performance baselines (architecture-specific, adjusted for CI/CD environments)
# CI/CD runners: shared CPU, variable load, thermal throttling
# Baseline = optimal performance (~10 us) * 2.5x CI overhead factor
# Dynamically select based on runner architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        # x86_64: SSE/AVX optimizations, ~10 us optimal -> 25 us CI acceptable
        BASELINE_BASE64_US=25
        ;;
    aarch64 | arm64)
        # ARM64: NEON optimizations, ~10 us optimal -> 25 us CI acceptable
        BASELINE_BASE64_US=25
        ;;
    *)
        # Other architectures: Conservative default (no SIMD assumed)
        BASELINE_BASE64_US=30
        ;;
esac

echo "========================================"
echo "  kitty-doom Performance Regression"
echo "========================================"
echo ""

# Ensure test binaries are built
if [ ! -f "build/tests/bench-base64" ]; then
    echo "Building test binaries..."
    make check > /dev/null 2>&1
fi

# Function to extract numeric value from output
extract_value()
{
    grep "$1" | sed -E 's/.*: +([0-9.]+).*/\1/'
}

# Function to compare performance
check_threshold()
{
    local name="$1"
    local value="$2"
    local baseline="$3"
    local unit="$4"

    # Use bc for floating point comparison
    if (($(echo "$value > $baseline" | bc -l))); then
        echo -e "${RED}[REGRESSION]${NC} $name: $value $unit (baseline: $baseline $unit)"
        return 1
    else
        echo -e "${GREEN}[OK]${NC} $name: $value $unit (baseline: $baseline $unit)"
        return 0
    fi
}

exit_code=0

# Base64 Encoding Performance
echo "=== Base64 Encoding (DOOM Framebuffer 192KB) ==="

base64_output=$(./build/tests/bench-base64 2>&1)

# Extract optimized implementation time
base64_time=$(echo "$base64_output" | grep "Average time:" | tail -1 | extract_value "Average time:")
base64_throughput=$(echo "$base64_output" | grep "Throughput:" | tail -1 | extract_value "Throughput:")
base64_speedup=$(echo "$base64_output" | grep "Speedup:" | tail -1 | extract_value "Speedup:")

echo "  Time:       $base64_time us/frame"
echo "  Throughput: $base64_throughput MB/s"
echo "  Speedup:    ${base64_speedup}x vs scalar"

if ! check_threshold "Base64 encoding" "$base64_time" "$BASELINE_BASE64_US" "us"; then
    exit_code=1
fi

echo ""

# Atomic Bitmap Correctness
echo "=== Atomic Bitmap (lock-free concurrent access) ==="

if ./build/tests/test-atomic-bitmap > /dev/null 2>&1; then
    echo -e "${GREEN}[OK]${NC} All atomic operations correct"
else
    echo -e "${RED}[FAIL]${NC} Atomic operations failed"
    exit_code=1
fi

echo ""

# Summary
echo "========================================"
if [ $exit_code -eq 0 ]; then
    echo -e "${GREEN}All performance tests PASSED${NC}"
else
    echo -e "${RED}Performance regression detected!${NC}"
    echo ""
    echo "If this is expected (e.g., new baseline after optimization),"
    echo "update the baseline values in $0"
fi
echo "========================================"

exit $exit_code
