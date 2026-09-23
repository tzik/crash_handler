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

# Run the test, combine stdout and stderr, and pipe to sed to sanitize dynamic parts.
# The following regexes sanitize:
# - Process IDs (e.g. "Process 12345 crashed")
# - Addresses (e.g. "0x7f8a9b..." or "0x123...")
# - Offsets (e.g. "+ 0x2464")
# - Frame index if it gets misaligned, but usually we just want to remove addr and offset.
# - File paths to libc or other system libraries if they contain version numbers.
# We will use simple regexes:
# 1. Strip 'Process [0-9]+' -> 'Process PID'
# 2. Strip '0x[0-9a-fA-F]+' -> 'ADDR'
# 3. Strip '\+ 0x[0-9a-fA-F]+' -> '+ OFFSET'

TMP_OUT=$(mktemp)

# Note: test_crash returns 0 on parent success even if child crashes,
# but we want to capture its output which happens synchronously.
"$TEST_EXE" "$WORKER_EXE" "$STRIP_PREFIX" > "$TMP_OUT" 2>&1 || true

SANITIZED_OUT=$(mktemp)
cat "$TMP_OUT" | \
    sed -E 's/Process [0-9]+/Process PID/g' | \
    sed -E 's/\+ 0x[0-9a-fA-F]+/ADDR/g' | \
    sed -E 's/0x[0-9a-fA-F]+/ADDR/g' > "$SANITIZED_OUT"

if [ ! -f "$EXPECTED_FILE" ]; then
    echo "Expected file $EXPECTED_FILE does not exist. Creating it based on current output."
    cp "$SANITIZED_OUT" "$EXPECTED_FILE"
fi

if diff -u "$EXPECTED_FILE" "$SANITIZED_OUT"; then
    echo "Output matches expectations."
    rm -f "$TMP_OUT" "$SANITIZED_OUT"
    exit 0
else
    echo "Output differs from expectations."
    rm -f "$TMP_OUT" "$SANITIZED_OUT"
    exit 1
fi
