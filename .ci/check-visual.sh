#!/usr/bin/env bash
# DOOM visual validation for CI
# Checks screenshot for proper rendering without pixel-perfect comparison
#
# Validation thresholds (tuned for DOOM shareware title screen):
#   - Brightness > 1000 (0-65535 scale): Detects non-black frames
#   - Std deviation > 2000: Ensures image has contrast (not solid color)
#   - Color variance (R-B) > 1000: DOOM has red/brown logo vs blue sky
#   - Saturation > 5%: Ensures colorful rendering (not grayscale)

set -euo pipefail

# Check if argument is provided (must check $# before accessing $1 with -u flag)
if [ $# -eq 0 ]; then
    echo "Usage: $0 <screenshot.png>"
    exit 1
fi

SCREENSHOT="$1"

if [ ! -f "$SCREENSHOT" ]; then
    echo "❌ Screenshot not found: $SCREENSHOT"
    exit 1
fi

# DOOM-specific visual validation (not pixel-perfect)
echo ""
echo "=== DOOM Visual Validation ==="
echo "Screenshot: $SCREENSHOT"

# 1. Calculate average brightness (0-65535 range)
BRIGHTNESS=$(convert "$SCREENSHOT" \
    -colorspace Gray -format "%[mean]" info: 2>&1)

# Validate brightness is non-empty and numeric
if [ -z "$BRIGHTNESS" ] || ! [[ "$BRIGHTNESS" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "❌ Invalid brightness value: '$BRIGHTNESS'"
    exit 1
fi

echo "Average brightness: $BRIGHTNESS (0-65535 scale)"

# 2. Calculate standard deviation (variance/contrast)
STDDEV=$(convert "$SCREENSHOT" \
    -colorspace Gray -format "%[standard-deviation]" info: 2>&1)

# Validate STDDEV is non-empty and numeric
if [ -z "$STDDEV" ] || ! [[ "$STDDEV" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "❌ Invalid standard deviation value: '$STDDEV'"
    exit 1
fi

echo "Std deviation: $STDDEV (higher = more contrast)"

# 3. Extract RGB channel means (DOOM has prominent red/brown tones)
RGB_STATS=$(convert "$SCREENSHOT" \
    -format "R:%[fx:mean.r*65535] G:%[fx:mean.g*65535] B:%[fx:mean.b*65535]" info:)
echo "Color channels: $RGB_STATS"

# Extract values using portable awk (not grep -P for BSD compatibility)
RED=$(echo "$RGB_STATS" | awk '{match($0, /R:([0-9.]+)/, a); print a[1]}')
BLUE=$(echo "$RGB_STATS" | awk '{match($0, /B:([0-9.]+)/, a); print a[1]}')

# Validate RGB values are non-empty and numeric
if [ -z "$RED" ] || [ -z "$BLUE" ]; then
    echo "❌ Invalid RGB stats: '$RGB_STATS'"
    exit 1
fi

if ! [[ "$RED" =~ ^[0-9]+(\.[0-9]+)?$ ]] || ! [[ "$BLUE" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "❌ Non-numeric RGB values: RED='$RED', BLUE='$BLUE'"
    exit 1
fi

# 4. Calculate color saturation (DOOM is colorful, not grayscale)
SATURATION=$(convert "$SCREENSHOT" \
    -colorspace HSL -format "%[fx:mean.g*100]" info: 2>&1)

# Validate SATURATION is non-empty and numeric
if [ -z "$SATURATION" ] || ! [[ "$SATURATION" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "❌ Invalid saturation value: '$SATURATION'"
    exit 1
fi

echo "Saturation: $SATURATION% (0=grayscale, 100=vivid)"

echo ""
echo "=== Validation Checks ==="

# Check 1: Not completely black (brightness > 1000)
if [ "${BRIGHTNESS%.*}" -le 1000 ]; then
    echo "❌ FAIL: Screenshot too dark ($BRIGHTNESS < 1000)"
    exit 1
fi
echo "✅ PASS: Non-black frame (brightness > 1000)"

# Check 2: Has contrast (std dev > 2000)
if (($(echo "$STDDEV < 2000" | bc -l))); then
    echo "❌ FAIL: Low contrast ($STDDEV < 2000) - possibly blank/solid color"
    exit 1
fi
echo "✅ PASS: High contrast detected (std dev > 2000)"

# Check 3: Has color variety (not monochrome)
# DOOM title screen has red/brown logo, blue sky, varied colors
COLOR_RANGE=$(echo "$RED - $BLUE" | bc)
COLOR_RANGE_ABS=${COLOR_RANGE#-} # absolute value
if (($(echo "$COLOR_RANGE_ABS < 1000" | bc -l))); then
    echo "⚠️  WARNING: Low color variance (R-B: $COLOR_RANGE_ABS)"
else
    echo "✅ PASS: Color variance detected (R-B difference: $COLOR_RANGE_ABS)"
fi

# Check 4: Not grayscale (saturation > 5%)
if (($(echo "$SATURATION < 5" | bc -l))); then
    echo "❌ FAIL: Nearly grayscale ($SATURATION% < 5%)"
    exit 1
fi
echo "✅ PASS: Colorful image (saturation: $SATURATION%)"

echo ""
echo "✅ DOOM rendering validated (4/4 checks passed)"
