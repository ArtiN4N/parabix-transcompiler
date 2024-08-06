# Install script for directory: /home/hla294/Desktop/cmpt479/parabix-devel

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/codegen/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/fileselect/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/grep/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/idisa/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/core/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/pipeline/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/objcache/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/pablo/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/pablo/bixnum/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/pablo/parse/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/adt/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/analysis/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/cc/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/compile/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/parse/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/printer/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/toolchain/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/transforms/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/re/unicode/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/testing/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/toolchain/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/unicode/algo/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/unicode/core/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/unicode/data/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/unicode/utf/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/basis/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/bitwise/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/io/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/scan/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/streamutils/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/unicode/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/util/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/lib/kernel/illustrator/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/base64/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/editd/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/icgrep/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/idisa_test/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/json/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/transcoders/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/wc/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/xml/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/ztf8/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/util/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tools/csv/cmake_install.cmake")
  include("/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/tests/cmake_install.cmake")

endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "/home/hla294/Desktop/cmpt479/parabix-devel/transliterator_compiler/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
