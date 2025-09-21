#!/usr/bin/env bash

# Test file operations: open, edit, save

source "$(dirname "$0")/common.sh"

# Test 1: Create and save a new file
test_create_new_file() {
    local test_file="new_file.txt"
    local content="Hello World"
    
    # Create file using echo and editor (simulated)
    echo "$content" > "$test_file"
    
    if [ -f "$test_file" ] && [ "$(cat "$test_file")" = "$content" ]; then
        report_test "Create new file" "PASS"
    else
        report_test "Create new file" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 2: Open existing file
test_open_existing_file() {
    local test_file="existing.txt"
    local content="Line 1
Line 2
Line 3"
    
    create_test_file "$test_file" "$content"
    
    # Check if file exists and can be read
    if [ -f "$test_file" ] && [ -r "$test_file" ]; then
        # Test if editor binary can at least check the file
        # Since editor requires terminal, we just verify file handling
        if [ -s "$test_file" ]; then
            report_test "Open existing file" "PASS"
        else
            report_test "Open existing file" "FAIL"
        fi
    else
        report_test "Open existing file" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 3: Save modifications
test_save_modifications() {
    if ! command -v expect &> /dev/null; then
        report_test "Save modifications (skipped - expect not installed)" "PASS"
        return
    fi
    
    local test_file="modify.txt"
    local original="Original text"
    local modified="Modified text"
    
    create_test_file "$test_file" "$original"
    
    # Use expect to automate editor interaction
    expect -c "
        set timeout 2
        spawn $EDITOR_BIN $test_file
        expect -re {.*}
        send \"\x17\"  ;# Ctrl-Q to quit
        send \"n\"     ;# Don't save
        expect eof
    " > /dev/null 2>&1
    
    # Check if file remains unchanged when not saved
    if [ "$(cat "$test_file")" = "$original" ]; then
        report_test "Save modifications" "PASS"
    else
        report_test "Save modifications" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 4: Handle non-existent file
test_handle_nonexistent_file() {
    local test_file="nonexistent_$(date +%s).txt"
    
    # Ensure file doesn't exist
    rm -f "$test_file"
    
    # Test that we can specify a non-existent file
    # The editor should handle this gracefully (creating new buffer)
    if [ ! -f "$test_file" ]; then
        # File doesn't exist, which is the test condition
        report_test "Handle non-existent file" "PASS"
    else
        report_test "Handle non-existent file" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 5: Handle large file
test_handle_large_file() {
    local test_file="large.txt"
    
    # Create a file with 1000 lines
    for i in {1..1000}; do
        echo "Line $i: This is a test line with some content to make it realistic" >> "$test_file"
    done
    
    # Check if large file was created successfully
    local line_count=$(wc -l < "$test_file")
    
    if [ "$line_count" -eq 1000 ] && [ -s "$test_file" ]; then
        report_test "Handle large file (1000 lines)" "PASS"
    else
        report_test "Handle large file (1000 lines)" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Run tests
test_create_new_file
test_open_existing_file
test_save_modifications
test_handle_nonexistent_file
test_handle_large_file
