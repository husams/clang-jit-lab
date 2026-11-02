#!/usr/bin/env bash
# Exercise the kernel's compiler, signature, integer and native-call boundaries.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/out"
CASE_DIR="$(mktemp -d "$ROOT/out/kernel-tests.XXXXXX")"
trap 'rm -rf "$CASE_DIR"' EXIT
BIN="$ROOT/build/bin/p07_kernel"

expect_failure() {
  local name="$1" message="$2"
  shift 2
  if "$BIN" "$@" >"$CASE_DIR/result" 2>&1; then
    echo "FAIL: $name unexpectedly succeeded"
    exit 1
  fi
  if ! rg -q -F "$message" "$CASE_DIR/result"; then
    echo "FAIL: $name did not report $message"
    cat "$CASE_DIR/result"
    exit 1
  fi
}

"$BIN" manifests/p07_kernel.cpp 0 6 -3 >"$CASE_DIR/result"
printf '%s\n' 'run(0) = 0' 'run(6) = 42' 'run(-3) = -21' >"$CASE_DIR/want"
diff -u "$CASE_DIR/want" "$CASE_DIR/result"

printf '%s\n' 'extern "C" double run(double x) { return x; }' >"$CASE_DIR/type.cpp"
printf '%s\n' 'extern "C" int other(int x) { return x; }' >"$CASE_DIR/missing.cpp"
printf '%s\n' 'extern "C" int run(int);' >"$CASE_DIR/declaration.cpp"
printf '%s\n' 'int run(int x) { return x; }' >"$CASE_DIR/linkage.cpp"
printf '%s\n' 'extern "C" int run(int x, ...) { return x; }' >"$CASE_DIR/variadic.cpp"
printf '%s\n' 'extern "C" int run(int x) { return ; }' >"$CASE_DIR/syntax.cpp"
printf '%s\n' 'extern "C" int absent(int); extern "C" int run(int x) { return absent(x); }' >"$CASE_DIR/external.cpp"
for name in type missing declaration linkage variadic; do
  expect_failure "$name" 'entry must be defined' "$CASE_DIR/$name.cpp"
done
expect_failure syntax 'parse:' "$CASE_DIR/syntax.cpp"
expect_failure unresolved 'lookup:' "$CASE_DIR/external.cpp"
expect_failure integer 'invalid integer' manifests/p07_kernel.cpp nope
expect_failure range 'invalid integer' manifests/p07_kernel.cpp 999999999999999999999
echo 'kernel tests: 10 cases passed'
