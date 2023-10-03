# This software is licensed to the public under the Open Software License 3.0.

# module: util

parabix_add_executable(
NAME
    hexlines
SRC
    hexlines.cpp
DEPS
    pablo
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.util
    toolchain
)


parabix_add_executable(
NAME
    hashdemo
SRC
    hashdemo.cpp
DEPS
    grep
    pablo
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.util
    toolchain
)

parabix_add_executable(
NAME
    hashtester
SRC
    hashtester.cpp
    murmur3/MurmurHash3.cpp
DEPS
    fileselect
    grep
    pablo
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.util
    toolchain
)

