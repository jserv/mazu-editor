#!/usr/bin/env bash

# Provides common functions for testing editor functionality

# Set up color configuration
set_colors() {
    if [ -t 1 ] && [ -n "$(tput colors 2> /dev/null)" ] && [ "$(tput colors)" -ge 8 ]; then
        RED='\033[0;31m'
        GREEN='\033[0;32m'
        YELLOW='\033[1;33m'
        NC='\033[0m' # No Color
    else
        RED=''
        GREEN=''
        YELLOW=''
        NC=''
    fi
}

# Initialize colors
set_colors

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test directories
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITOR_BIN="$TEST_DIR/me"
TEST_TMP_DIR="$TEST_DIR/test_tmp"

# Error handler
throw() {
    local message="$1"
    printf "${RED}Error: %s${NC}\n" "$message" >&2
    exit 1
}

# Create temporary test directory
setup_test_env() {
    mkdir -p "$TEST_TMP_DIR" || throw "Failed to create test directory"
    cd "$TEST_TMP_DIR" || throw "Failed to change to test directory"
}

# Clean up test environment
cleanup_test_env() {
    cd "$TEST_DIR" || return 1
    if [ -d "$TEST_TMP_DIR" ]; then
        rm -rf "$TEST_TMP_DIR"
    fi
}

# Report test result
report_test() {
    local test_name="$1"
    local result="$2"
    local message="${3:-}"

    if [ "$result" = "SKIP" ]; then
        printf "${YELLOW}⊘${NC} %s" "$test_name"
        [ -n "$message" ] && printf " (%s)" "$message"
        printf "\n"
        return
    fi

    TESTS_RUN=$((TESTS_RUN + 1))

    if [ "$result" = "PASS" ]; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
        printf "${GREEN}✓${NC} %s\n" "$test_name"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        printf "${RED}✗${NC} %s" "$test_name"
        [ -n "$message" ] && printf " (%s)" "$message"
        printf "\n"
    fi
}

# Print test summary
print_summary() {
    printf "\n===== Test Summary =====\n"
    printf "Total tests: %d\n" "$TESTS_RUN"
    printf "${GREEN}Passed: %d${NC}\n" "$TESTS_PASSED"
    if [ "$TESTS_FAILED" -gt 0 ]; then
        printf "${RED}Failed: %d${NC}\n" "$TESTS_FAILED"
        return 1
    else
        printf "${GREEN}All tests passed!${NC}\n"
        return 0
    fi
}

# Create a test file with content
create_test_file() {
    local filename="$1"
    local content="$2"
    printf "%s\n" "$content" > "$filename" || throw "Failed to create test file: $filename"
}

# Check if editor binary exists
check_editor_binary() {
    if [ ! -f "$EDITOR_BIN" ]; then
        throw "Editor binary not found at $EDITOR_BIN. Please build the editor first with 'make'"
    fi
}

# Send keystrokes to editor using expect
send_keys_to_editor() {
    local file="$1"
    local keys="$2"
    local timeout="${3:-2}"

    if ! command -v expect &> /dev/null; then
        return 1
    fi

    expect -c "
        set timeout $timeout
        spawn $EDITOR_BIN $file
        expect -re {.*}
        send \"$keys\"
        expect eof
    " > /dev/null 2>&1
}

# Compare two files
compare_files() {
    local file1="$1"
    local file2="$2"

    [ ! -f "$file1" ] && throw "File not found: $file1"
    [ ! -f "$file2" ] && throw "File not found: $file2"

    diff -q "$file1" "$file2" > /dev/null 2>&1
}

# Verify test prerequisites
verify_prerequisites() {
    check_editor_binary

    if ! command -v expect &> /dev/null; then
        printf "${YELLOW}Warning: 'expect' command not found${NC}\n"
        printf "Some interactive tests will be skipped\n"
        printf "Install with: brew install expect (macOS) or apt-get install expect (Linux)\n\n"
        return 1
    fi
    return 0
}

# Export functions for use in test scripts
export -f report_test
export -f create_test_file
export -f send_keys_to_editor
export -f compare_files
export -f throw
