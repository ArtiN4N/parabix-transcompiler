#!/bin/bash

# Check if exactly two arguments are provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <executable_name> <input file>"
    exit 1
fi

# Set the input argument to a variable
executable_name=$1
input=$2

# Define the path to the build directory
bin_dir="../../build/bin"

# Define the path to the input directory
input_dir="../tests"
current_dir=$(pwd)

# Check if the executable exists in the bin directory
if [ -x "$bin_dir/$executable_name" ]; then
    # Run the executable
    "$bin_dir/$executable_name" "$current_dir/$input_dir/$2"
else
    echo "Executable $executable_name not found in $bin_dir or is not executable."
    exit 1
fi