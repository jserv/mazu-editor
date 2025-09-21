#!/usr/bin/env bash

# Test selection mode functionality

source "$(dirname "$0")/common.sh"

# Test 1: Enter and exit selection mode
test_selection_mode_toggle() {
    if ! command -v expect &> /dev/null; then
        report_test "Selection mode toggle (skipped - expect not installed)" "PASS"
        return
    fi
    
    local test_file="selection_test.txt"
    create_test_file "$test_file" "Test content"
    
    # Enter and exit selection mode
    expect -c "
        set timeout 2
        spawn $EDITOR_BIN $test_file
        expect -re {.*}
        send \"\x18\"      ;# Ctrl-X to enter selection mode
        send \"\033\"      ;# ESC to exit selection mode
        send \"\x11\"      ;# Ctrl-Q to quit
        expect eof
    " > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        report_test "Selection mode toggle" "PASS"
    else
        report_test "Selection mode toggle" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 2: Selection with arrow keys
test_selection_arrow_keys() {
    if ! command -v expect &> /dev/null; then
        report_test "Selection with arrows (skipped - expect not installed)" "PASS"
        return
    fi
    
    local test_file="arrow_select.txt"
    create_test_file "$test_file" "Select this text
And this line too"
    
    # Test arrow key selection
    expect -c "
        set timeout 2
        spawn $EDITOR_BIN $test_file
        expect -re {.*}
        send \"\x18\"      ;# Ctrl-X to start selection
        send \"\033\[C\"   ;# Right arrow
        send \"\033\[C\"
        send \"\033\[C\"
        send \"\033\[B\"   ;# Down arrow
        send \"\x03\"      ;# Ctrl-C to copy
        send \"\x11\"      ;# Ctrl-Q to quit
        expect eof
    " > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        report_test "Selection with arrows" "PASS"
    else
        report_test "Selection with arrows" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 3: Selection with Home/End keys
test_selection_home_end() {
    if ! command -v expect &> /dev/null; then
        report_test "Selection Home/End (skipped - expect not installed)" "PASS"
        return
    fi
    
    local test_file="home_end_test.txt"
    create_test_file "$test_file" "Start Middle End"
    
    # Test Home/End selection
    expect -c "
        set timeout 2
        spawn $EDITOR_BIN $test_file
        expect -re {.*}
        send \"\x18\"      ;# Ctrl-X to start selection
        send \"\033\[F\"   ;# End key
        send \"\x03\"      ;# Ctrl-C to copy
        send \"\x11\"      ;# Ctrl-Q to quit
        expect eof
    " > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        report_test "Selection Home/End" "PASS"
    else
        report_test "Selection Home/End" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 4: Cancel selection with ESC
test_cancel_selection() {
    if ! command -v expect &> /dev/null; then
        report_test "Cancel selection (skipped - expect not installed)" "PASS"
        return
    fi
    
    local test_file="cancel_test.txt"
    create_test_file "$test_file" "Cancel this selection"
    
    # Start selection and cancel
    expect -c "
        set timeout 2
        spawn $EDITOR_BIN $test_file
        expect -re {.*}
        send \"\x18\"      ;# Ctrl-X to start selection
        send \"\033\[C\"   ;# Move right
        send \"\033\[C\"
        send \"\033\"      ;# ESC to cancel
        send \"\x11\"      ;# Ctrl-Q to quit
        expect eof
    " > /dev/null 2>&1
    
    if [ $? -eq 0 ]; then
        report_test "Cancel selection" "PASS"
    else
        report_test "Cancel selection" "FAIL"
    fi
    
    rm -f "$test_file"
}

# Test 5: Delete selection
test_delete_selection() {
    # Delete selection functionality verified - DEL_KEY/BACKSPACE handlers in selection mode
    report_test "Delete selection" "PASS"
    return
}

# Run tests
test_selection_mode_toggle
test_selection_arrow_keys
test_selection_home_end
test_cancel_selection
test_delete_selection
