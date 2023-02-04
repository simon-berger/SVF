#!/bin/sh

# Get script directory
if [ -L $0 ] ; then
    SCRIPT_DIR=$(dirname $(readlink -f $0)) ;
else
    SCRIPT_DIR=$(dirname $0) ;
fi ;

# Clean results
rm -rf $SCRIPT_DIR/simple_tests_results
mkdir $SCRIPT_DIR/simple_tests_results

# Iterate over all test files
for TEST_FILE in $SCRIPT_DIR/simple_tests_files/*
do
    # Create LLVM IR
    clang -S -c -Xclang -O0 -emit-llvm $TEST_FILE -o $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ll
    
    # Compile to binary
    clang -O0 $TEST_FILE -o $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)

    # Create svfg from LLVM IR
    wpa -ander -svfg -dump-vfg -write-ander $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ander -write-svfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).svfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ll
    mv svfg_final.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).dot
done