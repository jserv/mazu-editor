#!/usr/bin/env bash

# Runs all test suites and reports results

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source test utilities
source "$SCRIPT_DIR/common.sh"

# Set test directory to script directory (override test-utils.sh)
TEST_DIR="$SCRIPT_DIR"

echo "===== Mazu Editor Test Suite ====="
echo

# Check prerequisites
check_editor_binary

# Check for expect command
if ! command -v expect &>/dev/null; then
    echo -e "${YELLOW}Warning: 'expect' command not found${NC}"
    echo "Some interactive tests will be skipped"
    echo "Install with: brew install expect (macOS) or apt-get install expect (Linux)"
    echo
fi

# Setup test environment
setup_test_env

# Run test suites and capture output for counting
echo "Running test suites..."
echo

# Capture all test output to a temporary file for counting
ALL_OUTPUT=$(mktemp)
trap "rm -f $ALL_OUTPUT" EXIT

{
    # Basic file operations tests
    if [ -f "$TEST_DIR/test-file_operations.sh" ]; then
        echo "=== File Operations Tests ==="
        bash "$TEST_DIR/test-file_operations.sh"
        echo
    fi

    # Copy/paste tests
    if [ -f "$TEST_DIR/test-copy_paste.sh" ]; then
        echo "=== Copy/Paste Tests ==="
        bash "$TEST_DIR/test-copy_paste.sh"
        echo
    fi

    # Selection mode tests
    if [ -f "$TEST_DIR/test-selection.sh" ]; then
        echo "=== Selection Mode Tests ==="
        bash "$TEST_DIR/test-selection.sh"
        echo
    fi

    # Undo/redo tests
    if [ -f "$TEST_DIR/test-undo_redo.sh" ]; then
        echo "=== Undo/Redo Tests ==="
        bash "$TEST_DIR/test-undo_redo.sh"
        echo
    fi

    # Search functionality tests
    if [ -f "$TEST_DIR/test-search.sh" ]; then
        echo "=== Search Tests ==="
        bash "$TEST_DIR/test-search.sh"
        echo
    fi

    # File browser tests
    if [ -f "$TEST_DIR/test-file_browser.sh" ]; then
        echo "=== File Browser Tests ==="
        bash "$TEST_DIR/test-file_browser.sh"
        echo
    fi

    # Line numbers toggle tests
    if [ -f "$TEST_DIR/test-line_numbers.sh" ]; then
        echo "=== Line Numbers Tests ==="
        bash "$TEST_DIR/test-line_numbers.sh"
        echo
    fi
} | tee "$ALL_OUTPUT"

# Cleanup
cleanup_test_env

# Count results from the actual output that was displayed
TOTAL_TESTS=$(grep -c -E "✓|✗|⊘" "$ALL_OUTPUT" 2>/dev/null || echo 0)
PASSED_TESTS=$(grep -c "✓" "$ALL_OUTPUT" 2>/dev/null || echo 0)
FAILED_TESTS=$(grep -c "✗" "$ALL_OUTPUT" 2>/dev/null || echo 0)

# Clean up any newlines in variables
TOTAL_TESTS=$(echo "$TOTAL_TESTS" | tr -d '\n')
PASSED_TESTS=$(echo "$PASSED_TESTS" | tr -d '\n')
FAILED_TESTS=$(echo "$FAILED_TESTS" | tr -d '\n')

echo
echo "===== Test Summary ====="
echo "Total tests: $TOTAL_TESTS"

if [ "$FAILED_TESTS" -eq 0 ]; then
    echo -e "${GREEN}Passed: $PASSED_TESTS${NC}"
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${GREEN}Passed: $PASSED_TESTS${NC}"
    echo -e "${RED}Failed: $FAILED_TESTS${NC}"
    exit 1
fi
