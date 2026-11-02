#!/usr/bin/env bash
# Everything the lab promises, in one command.
#
#   scripts/check.sh           clean build, smoke test, doccheck, links, site
#   scripts/check.sh --quick   the same without the clean rebuild
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "== complete C++ lesson sources and platform commands"
python3 scripts/complete_examples.py

if [[ "${1:-}" == "--quick" ]]; then
  scripts/build.sh
else
  scripts/build.sh --clean
fi

echo "== smoke"
build/bin/p00_smoke

echo "== kernel host tests"
scripts/test_kernel.sh

echo "== inspector pointer tests"
scripts/test_inspector.sh

echo "== supporting library fixture"
mkdir -p out
if [[ "$(uname -s)" == Darwin ]]; then
  LAB_LLVM="${LLVM:-$(brew --prefix llvm)}"
  "$LAB_LLVM/bin/clang++" -dynamiclib manifests/p03_library.cpp -o out/p03_library.dylib
else
  LAB_LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
  "$LAB_LLVM/bin/clang++" -fPIC -shared manifests/p03_library.cpp -o out/p03_library.so
  build/bin/p03_library out/p03_library.so
fi

if [[ "$(uname -s)" == Darwin ]]; then
  echo "== xrepl reference tests"
  scripts/test_xrepl.sh
  echo "== recorded macOS output"
  python3 scripts/doccheck.py docs/part_*.md
else
  echo "== Linux API regression suite"
  scripts/test_linux.sh
  echo "Linux behavior checks complete; macOS reference transcripts are compared on macOS."
fi

echo "== links"
python3 scripts/check_links.py

echo "== site"
python3 scripts/build_site.py
python3 scripts/check_site.py
echo "check.sh: all checks passed"
