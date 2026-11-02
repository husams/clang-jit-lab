cmake_minimum_required(VERSION 3.20)

# Find LLVM 22 so a plain `cmake -S . -B build` works; set $LLVM to override.
# This must run before project(), which locks in the compiler.
foreach(_llvm "$ENV{LLVM}" /opt/homebrew/opt/llvm /usr/local/opt/llvm /usr/lib/llvm-22)
  if(_llvm AND EXISTS "${_llvm}/bin/clang++")
    list(PREPEND CMAKE_PREFIX_PATH "${_llvm}")
    if(NOT CMAKE_CXX_COMPILER)
      set(CMAKE_C_COMPILER "${_llvm}/bin/clang")
      set(CMAKE_CXX_COMPILER "${_llvm}/bin/clang++")
    endif()
    break()
  endif()
endforeach()
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type")

# C is enabled too: LLVM's CMake modules run C header probes.
project(jit_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Clang C++ APIs change between releases, so require the 22.1 series.
find_package(LLVM 22.1 REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

add_executable(jit_host src/main.cpp)

target_include_directories(jit_host SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS})

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_definitions(jit_host PRIVATE ${LLVM_DEFINITIONS_LIST})

# Must match LLVM, or subclassing its classes fails to link (missing typeinfo).
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(jit_host PRIVATE -fno-rtti)
endif()

# Monolithic dylibs; together they contain clangInterpreter and ORC.
target_link_libraries(jit_host PRIVATE clang-cpp LLVM)

# Find the dylibs at run time without DYLD_* variables.
set_target_properties(jit_host PROPERTIES BUILD_RPATH "${LLVM_INSTALL_PREFIX}/lib")
