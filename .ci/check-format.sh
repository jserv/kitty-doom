#!/usr/bin/env bash
# Code formatting check for CI/CD
# Uses clang-format-21 from apt.llvm.org for consistency

set -u -o pipefail

ret=0

# Select clang-format binary (prefer direct path in CI to avoid version ambiguity)
if [ -x "/usr/bin/clang-format-21" ]; then
    CLANG_FORMAT="/usr/bin/clang-format-21"
else
    CLANG_FORMAT="clang-format"
fi

echo "Checking C/C++ code formatting (using: $CLANG_FORMAT)..."

# Format C files with clang-format, then verify no changes
# CI's clang-format-21 is the single source of truth
while IFS= read -r -d '' file; do
    "$CLANG_FORMAT" -i "${file}"
done < <(git ls-files -z '*.c' '*.h' ':!:src/PureDOOM.h' ':!:src/miniaudio.h')

if ! git diff --exit-code --quiet; then
    echo "Error: Code formatting changes detected."
    echo "Run 'make indent' locally to fix formatting."
    echo ""
    echo "=== Formatting differences ==="
    git diff
    ret=1
else
    echo "✓ C/C++ formatting OK"
fi

echo ""
echo "Checking shell script formatting..."

# Format shell scripts with shfmt (if available)
# shfmt automatically honors .editorconfig settings
if command -v shfmt &> /dev/null; then
    shfmt_failed=0
    while IFS= read -r file; do
        if ! diff -q <(cat "${file}") <(shfmt "${file}") > /dev/null 2>&1; then
            echo "Error: ${file} needs formatting"
            echo ""
            echo "=== Differences in ${file} ==="
            diff -u <(cat "${file}") <(shfmt "${file}") || true
            echo ""
            shfmt_failed=1
        fi
    done < <(git ls-files '*.sh')

    if [ $shfmt_failed -eq 0 ]; then
        echo "✓ Shell script formatting OK"
    else
        echo "Run 'shfmt -w <file>' to fix (honors .editorconfig)"
        ret=1
    fi
else
    echo "Warning: shfmt not found, skipping shell script format check"
fi

exit ${ret}
