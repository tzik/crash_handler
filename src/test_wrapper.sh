#!/bin/bash
set -euo pipefail

if [ "$#" -lt 4 ]; then
  echo "Usage: $0 <test_executable> <worker_executable> <strip_path_prefix> <expected_file>"
  exit 1
fi

test_exe="$1"
worker_exe="$2"
strip_prefix="$3"
expected_file="$4"

# Find FileCheck
filecheck=""
if command -v FileCheck >/dev/null 2>&1; then
  filecheck="FileCheck"
else
  # Look in common LLVM installation paths
  for p in /usr/lib/llvm-*/bin/FileCheck; do
    if [ -x "$p" ]; then
      filecheck="$p"
      break
    fi
  done
fi

if [ -z "$filecheck" ]; then
  echo "Error: FileCheck not found. Please install llvm package."
  exit 1
fi

tmp_out="$(mktemp)"
# Run the test, combine stdout and stderr.
"$test_exe" "$worker_exe" "$strip_prefix" > "$tmp_out" 2>&1 || true

if "$filecheck" "$expected_file" < "$tmp_out"; then
  echo "Output matches expectations."
  rm -f "$tmp_out"
  exit 0
else
  echo "Output differs from expectations."
  cat "$tmp_out"
  rm -f "$tmp_out"
  exit 1
fi
