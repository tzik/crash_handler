#!/bin/bash
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "Usage: $0 <test_executable> <worker_executable> <strip_path_prefix> <expected_file>"
    exit 1
fi

TEST_EXE="$1"
WORKER_EXE="$2"
STRIP_PREFIX="$3"
EXPECTED_FILE="$4"

# Find FileCheck
FILECHECK=""
if command -v FileCheck >/dev/null 2>&1; then
    FILECHECK="FileCheck"
else
    # Look in common LLVM installation paths
    for p in /usr/lib/llvm-*/bin/FileCheck; do
        if [ -x "$p" ]; then
            FILECHECK="$p"
            break
        fi
    done
fi

if [ -z "$FILECHECK" ]; then
    echo "Error: FileCheck not found. Please install llvm package."
    exit 1
fi

TMP_OUT=$(mktemp)
# Run the test, combine stdout and stderr.
"$TEST_EXE" "$WORKER_EXE" "$STRIP_PREFIX" > "$TMP_OUT" 2>&1 || true

if "$FILECHECK" "$EXPECTED_FILE" < "$TMP_OUT"; then
    echo "Output matches expectations."
    rm -f "$TMP_OUT"
    exit 0
else
    echo "Output differs from expectations."
    cat "$TMP_OUT"
    rm -f "$TMP_OUT"
    exit 1
fi
