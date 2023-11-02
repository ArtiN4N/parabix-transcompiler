# Copyright (c) 2019 International Characters.
# This software is licensed to the public under the Open Software License 3.0.

# module: wc

parabix_add_executable(
NAME
    csv_validator
SRC
    csv_validator.cpp
    csv_schema_validator.cpp
    simple_csv_schema_parser.cpp
DEPS
    fileselect
    kernel.basis
    kernel.io
    re.cc
)
