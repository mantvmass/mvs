#!/bin/bash

# run_tests.sh - Script to run MVS compiler test cases

TEST_DIR="tests"
EXEC="./mvs"
TEST_OUTPUT="mvs_program"
TEST_CASES=(
    "add.mvs:15"
    "sub.mvs:13"
    "mul.mvs:18"
    "div.mvs:12"
    "mod.mvs:2"
    "pow.mvs:8"
    "paren.mvs:15"
    "complex.mvs:6"
)

passed=0
total=0

echo "Running all test cases in $TEST_DIR..."

for test in "${TEST_CASES[@]}"; do
    test_file=$(echo "$test" | cut -d: -f1)
    expected=$(echo "$test" | cut -d: -f2)
    test_path="$TEST_DIR/$test_file"
    
    total=$((total + 1))
    
    if [ ! -f "$test_path" ]; then
        echo "Error: Test file $test_path not found"
        continue
    fi
    
    echo "Testing $test_path (expected exit code $expected)..."
    $EXEC < "$test_path"
    
    if [ ! -f "$TEST_OUTPUT" ]; then
        echo "Error: $TEST_OUTPUT was not created for $test_path"
        continue
    fi
    
    ./"$TEST_OUTPUT"
    exit_code=$?
    
    if [ "$exit_code" -eq "$expected" ]; then
        echo "PASS: $test_path returned $exit_code"
        passed=$((passed + 1))
    else
        echo "FAIL: $test_path returned $exit_code, expected $expected"
    fi
done

echo "Test results: $passed/$total passed"
if [ "$passed" -ne "$total" ]; then
    exit 1
fi
