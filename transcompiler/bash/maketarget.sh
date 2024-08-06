#!/bin/bash

# Check if exactly one argument is provided
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <target_name>"
    exit 1
fi

# Set the input argument to a variable
target_name=$1

# Define the path to the build directory
build_dir="../../build"
orig_dir=$(pwd)

cd "$build_dir"
make "$target_name"
cd "$orig_dir"