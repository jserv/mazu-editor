#!/usr/bin/env bash

# The -e is not set because we want to get all the mismatch format at once

set -u -o pipefail

set -x

REPO_ROOT="$(git rev-parse --show-toplevel)"

C_SOURCES=$(find "${REPO_ROOT}" -name "*.c" -o -name "*.h" | grep -v "tests/test-features.c")
for file in ${C_SOURCES}; do
    clang-format ${file} > expected-format
    diff -u -p --label="${file}" --label="expected coding style" ${file} expected-format
done
C_MISMATCH_LINE_CNT=$(clang-format --output-replacements-xml ${C_SOURCES} | egrep -c "</replacement>" || echo 0)

SH_SOURCES=$(find "${REPO_ROOT}" -name "*.sh")
if command -v shfmt &> /dev/null; then
    for file in ${SH_SOURCES}; do
        shfmt -d "${file}"
    done
    SH_MISMATCH_FILE_CNT=$(shfmt -l ${SH_SOURCES} | wc -l)
else
    echo "shfmt not found, skipping shell script format check"
    SH_MISMATCH_FILE_CNT=0
fi

exit $((C_MISMATCH_LINE_CNT + SH_MISMATCH_FILE_CNT))