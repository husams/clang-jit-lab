# Shared build for every C++ tool in the clang-repl lab (Parts 4-7).
#
# Configure & build (from the lab root):
#   scripts/build.sh                 # all tools
#   scripts/build.sh p04_hello       # just one
#   cmake -S . -B build              # plain CMake, from tools/
#
# Adding a tool needs NO edit here: create tools/pNN_<name>/ containing one or
# more .cpp files and re-run scripts/build.sh. Every such directory becomes an
# executable called pNN_<name> in build/bin/.
#
# A directory may carry its own CMakeLists.txt to override the default; if it
# does, it should call add_repl_tool(<name> <sources...>) itself.

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
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

# C is enabled too: LLVM's CMake modules run C header probes (FindLibEdit).
project(clang_repl_lab C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "clang-repl-lab: LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
  message(WARNING "clang-repl-lab was written against LLVM 22; found ${LLVM_PACKAGE_VERSION}. "
                  "Expect API drift (clang/Interpreter changes in every release).")
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/common")

# Paths baked into repllab.h (platformArgs). LLVM_INSTALL_PREFIX is the real
# (versioned Cellar) path; scripts/build.sh re-runs CMake on every call, so a
# `brew upgrade llvm` is picked up on the next build.
set(REPLLAB_LLVM_PREFIX "${LLVM_INSTALL_PREFIX}")
# The lab root (parent of tools/), for tools that open manifests/ or out/ files.
get_filename_component(REPLLAB_LAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
  OUTPUT_VARIABLE REPLLAB_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "clang-repl-lab: resource dir ${REPLLAB_RESOURCE_DIR}")
configure_file(common/repllab_platform.h.in generated/repllab_platform.h @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

# Tools must match LLVM's RTTI setting or subclassing LLVM/Clang classes fails to
# link (missing typeinfo). Homebrew's LLVM 22 is built WITH RTTI
# (LLVMConfig.cmake: LLVM_ENABLE_RTTI ON), so this guard is a no-op there.
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
add_compile_options(-Wall -Wno-unused-parameter)

# add_repl_tool(<name> <source>...)
#   One executable linked against the monolithic libclang-cpp and libLLVM
#   dylibs (they contain clangInterpreter and ORC). Output: build/bin/<name>.
#   The executable's own global symbols stay exported (the macOS default), so
#   code running in the JIT can call functions defined in the tool (Part 5).
function(add_repl_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE clang-cpp LLVM)
  # ELF executables must export callbacks and globals used by JIT code.
  if(UNIX AND NOT APPLE)
    target_link_options(${name} PRIVATE -Wl,--export-dynamic)
  endif()
  target_compile_definitions(${name} PRIVATE
    REPLLAB_LLVM_PREFIX="${REPLLAB_LLVM_PREFIX}"
    REPLLAB_RESOURCE_DIR="${REPLLAB_RESOURCE_DIR}"
    REPLLAB_TOOL_NAME="${name}"
    REPLLAB_LAB_ROOT="${REPLLAB_LAB_ROOT}")
  # Run the dylibs from the Homebrew prefix without DYLD_* variables.
  set_target_properties(${name} PROPERTIES
    BUILD_RPATH "${LLVM_INSTALL_PREFIX}/lib"
    INSTALL_RPATH "${LLVM_INSTALL_PREFIX}/lib")
endfunction()

# Auto-discover tools/pNN_<name>/ directories.
file(GLOB _tool_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/p[0-9][0-9]_*")
list(SORT _tool_dirs)
foreach(dir ${_tool_dirs})
  if(NOT IS_DIRECTORY "${dir}")
    continue()
  endif()
  get_filename_component(_name "${dir}" NAME)
  if(EXISTS "${dir}/CMakeLists.txt")
    add_subdirectory("${dir}" "${CMAKE_CURRENT_BINARY_DIR}/${_name}")
  else()
    file(GLOB _srcs "${dir}/*.cpp")
    list(SORT _srcs)
    if(_srcs)
      add_repl_tool(${_name} ${_srcs})
    endif()
  endif()
endforeach()
