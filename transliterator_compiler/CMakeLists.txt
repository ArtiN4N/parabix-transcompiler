cmake_minimum_required(VERSION 3.10)
project(TransliteratorCompiler)

set(CMAKE_CXX_STANDARD 11)

# Add the include directory for the project's headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add the include directory for Parabix headers
include_directories(/home/hla294/Desktop/cmpt479/parabix-devel/include)

# Add the include directory for LLVM headers
include_directories(/usr/lib/llvm-14/include)  # Correct path for LLVM headers

# Add the library directory for Parabix libraries
link_directories(/home/hla294/Desktop/cmpt479/parabix-devel/lib)

# Add the library directory for LLVM libraries
link_directories(/usr/lib/llvm-14/lib) 

add_executable(TransliteratorCompiler 
    main.cpp 
    src/compiler.cpp 
    src/transform_kernel.cpp
    src/kernel_calls.cpp)

target_link_libraries(TransliteratorCompiler parabix LLVM)
