cmake_minimum_required(VERSION 3.10)
project(TransliteratorCompiler)

set(CMAKE_CXX_STANDARD 11)

include_directories(include)

add_executable(TransliteratorCompiler 
    main.cpp 
    src/icu_parser.cpp 
    src/kernel_calls.cpp 
    src/compiler.cpp)