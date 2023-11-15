# Copyright (c) 2019 International Characters.
# This software is licensed to the public under the Open Software License 3.0.

# module: wc

parabix_add_executable(
NAME
    csv_validator
SRC
    csv_error_identifier.cpp
    csv_validator.cpp
    csv_schema_validator.cpp
    csv_matcher_engine.cpp
    csv_validator_util.cpp
    check_hash_table.cpp
    simple_csv_schema_parser.cpp
    ../csv/csv_util.cpp
DEPS
    pablo
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.util
    fileselect
    kernel.basis
    kernel.io
    re.cc
    re.analysis
    re.parse
)
