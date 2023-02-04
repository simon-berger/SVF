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

    # Create graphs and data from LLVM IR using SVF
    wpa -nander -svfg -dump-vfg -dump-callgraph -dump-pag -write-ander $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ander -write-svfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).svfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ll
    mv svfg_final.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_svfg.dot
    mv callgraph_final.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_callgraph.dot
    mv svfir_initial.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_svfir.dot
    rm callgraph_initial.dot
    wpa -type -dump-icfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ll
    mv icfg_initial.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_icfg.dot
    mv vfg_initial.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_vfg.dot
    
    saber -nander -dump-free -dump-vfg $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c).ll
    mv svfg_final.dot $SCRIPT_DIR/simple_tests_results/$(basename "$TEST_FILE" .c)_saber_svfg.dot
done