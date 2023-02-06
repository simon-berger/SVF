#!/bin/bash

# Get script directory
if [ -L $0 ] ; then
    SCRIPT_DIR=$(dirname $(readlink -f $0)) ;
else
    SCRIPT_DIR=$(dirname $0) ;
fi ;

# Clean results
rm -rf ${SCRIPT_DIR}/simple_tests_results
mkdir ${SCRIPT_DIR}/simple_tests_results

# Iterate over all test files
for TEST_FILE in ${SCRIPT_DIR}/simple_tests_files/*
do
    # Get base name of test
    TEST_NAME=$(basename "${TEST_FILE}" .c)

    # Create directory for test results
    TEST_DIR=${SCRIPT_DIR}/simple_tests_results/${TEST_NAME}
    mkdir ${TEST_DIR}

    # Create LLVM IR
    clang -S -c -Xclang -O0 -emit-llvm ${TEST_FILE} -o ${TEST_DIR}/${TEST_NAME}.ll
    
    # Compile to binary
    clang -O0 ${TEST_FILE} -o ${TEST_DIR}/${TEST_NAME}.bin

    # Create graphs and data from LLVM IR using SVF
    wpa -nander -svfg -show-hidden-nodes -svfg-with-ind-call -rp-td-edge -dump-vfg -dump-callgraph -dump-pag -write-ander ${TEST_DIR}/${TEST_NAME}_wpa.ander -write-svfg ${TEST_DIR}/${TEST_NAME}_wpa.svfg ${TEST_DIR}/${TEST_NAME}.ll
    mv svfg_final.dot ${TEST_DIR}/${TEST_NAME}_wpa_svfg.dot
    mv callgraph_final.dot ${TEST_DIR}/${TEST_NAME}_wpa_callgraph.dot
    mv svfir_initial.dot ${TEST_DIR}/${TEST_NAME}_wpa_svfir.dot
    rm callgraph_initial.dot
    wpa -type -dump-icfg ${TEST_DIR}/${TEST_NAME}.ll
    mv icfg_initial.dot ${TEST_DIR}/${TEST_NAME}_wpa_icfg.dot
    mv vfg_initial.dot ${TEST_DIR}/${TEST_NAME}_wpa_vfg.dot
done