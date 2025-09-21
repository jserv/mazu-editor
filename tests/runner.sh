#!/usr/bin/env bash

# Runs all test suites and reports results

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Source test utilities
source "$SCRIPT_DIR/common.sh"

# Set test directory to script directory (override test-utils.sh)
TEST_DIR="$SCRIPT_DIR"

# Run a single test suite if it exists
run_test_suite() {
    local test_file="$1"
    local test_name="$2"

    if [ -f "$TEST_DIR/$test_file" ]; then
        echo "=== $test_name ==="
        "$TEST_DIR/$test_file"
        echo
    fi
}

# Define test suites with their display names
declare -A TEST_SUITES=(
    ["test-file_operations.sh"]="File Operations Tests"
    ["test-copy_paste.sh"]="Copy/Paste Tests"
    ["test-selection.sh"]="Selection Mode Tests"
    ["test-undo_redo.sh"]="Undo/Redo Tests"
    ["test-search.sh"]="Search Tests"
    ["test-file_browser.sh"]="File Browser Tests"
    ["test-line_numbers.sh"]="Line Numbers Tests"
)

echo "===== Mazu Editor Test Suite ====="
echo

# Check prerequisites
check_editor_binary

# Check for expect command
if ! command -v expect &> /dev/null; then
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
    # Run all test suites
    for test_file in "test-file_operations.sh" "test-copy_paste.sh" "test-selection.sh" \
                     "test-undo_redo.sh" "test-search.sh" "test-file_browser.sh" \
                     "test-line_numbers.sh"; do
        run_test_suite "$test_file" "${TEST_SUITES[$test_file]}"
    done
} | tee "$ALL_OUTPUT"

# Cleanup
cleanup_test_env

# Count test results from output
count_test_results() {
    local pattern="$1"
    local count=$(grep -c "$pattern" "$ALL_OUTPUT" 2>/dev/null || echo 0)
    echo "$count" | tr -d '\n'
}

# Generate test summary report
print_test_summary() {
    local total="$1"
    local passed="$2"
    local failed="$3"

    echo
    echo "===== Test Summary ====="
    echo "Total tests: $total"

    if [ "$failed" -eq 0 ]; then
        echo -e "${GREEN}Passed: $passed${NC}"
        echo -e "${GREEN}All tests passed!${NC}"
        return 0
    else
        echo -e "${GREEN}Passed: $passed${NC}"
        echo -e "${RED}Failed: $failed${NC}"
        return 1
    fi
}

# Count results
TOTAL_TESTS=$(count_test_results "✓\|✗\|⊘")
PASSED_TESTS=$(count_test_results "✓")
FAILED_TESTS=$(count_test_results "✗")

# Print summary and exit with appropriate code
print_test_summary "$TOTAL_TESTS" "$PASSED_TESTS" "$FAILED_TESTS"
exit $?
