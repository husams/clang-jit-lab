#!/usr/bin/env bash
# Build the lab's C++ tools (tools/pNN_*/ -> build/bin/pNN_*).
#
#   scripts/build.sh                 configure + build everything
#   scripts/build.sh p04_hello ...   configure + build the named tools only
#   scripts/build.sh --list          list the tool names
#   scripts/build.sh --clean         delete build/ first, then build everything
#
# CMake is re-run on every call so that a new tools/pNN_<name>/ directory (or a
# `brew upgrade llvm`) is picked up without a manual step. Concurrent calls are
# serialised with a lock directory, so two terminals (or two authors) can run
# it at the same time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${LLVM:-}" ]]; then
  if [[ "$(uname -s)" == Darwin ]] && command -v brew >/dev/null 2>&1; then
    LLVM="$(brew --prefix llvm)"
  elif command -v llvm-config-22 >/dev/null 2>&1; then
    LLVM="$(llvm-config-22 --prefix)"
  elif command -v llvm-config >/dev/null 2>&1; then
    LLVM="$(llvm-config --prefix)"
  else
    echo 'Set LLVM to the installation prefix of LLVM 22 (containing bin/clang++).'
    exit 1
  fi
fi
BUILD="$ROOT/build"

if [[ "${1:-}" == "--list" ]]; then
  for d in "$ROOT"/tools/p[0-9][0-9]_*/; do basename "$d"; done
  exit 0
fi

LOCK="$ROOT/.build.lock"
until mkdir "$LOCK" 2>/dev/null; do sleep 0.5; done
trap 'rmdir "$LOCK"' EXIT

if [[ "${1:-}" == "--clean" ]]; then
  rm -rf "$BUILD"
  shift
fi

cmake -S "$ROOT/tools" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" \
  -DLLVM_DIR="$LLVM/lib/cmake/llvm" \
  -DClang_DIR="$LLVM/lib/cmake/clang" >"$BUILD.cmake.log" 2>&1 || {
  cat "$BUILD.cmake.log"
  exit 1
}

if [[ $# -gt 0 ]]; then
  ninja -C "$BUILD" "$@"
else
  ninja -C "$BUILD"
fi
