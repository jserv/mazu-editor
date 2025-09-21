#!/usr/bin/env bash

set -e -u -o pipefail

# Run cppcheck without error-exitcode and check output manually
OUTPUT=$(cppcheck --enable=warning,performance,portability \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=objectIndex \
    --quiet \
    me.c 2>&1)

# Display any output for debugging
if [ -n "$OUTPUT" ]; then
    echo "Cppcheck output:"
    echo "$OUTPUT"
fi

# Only fail on actual errors or warnings (not information messages)
if echo "$OUTPUT" | grep -E "(error:|warning:)" > /dev/null; then
    echo "Found errors or warnings in cppcheck output"
    exit 1
fi

exit 0
