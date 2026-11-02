#!/usr/bin/env bash
# Run the golden-transcript tests of the p07_xrepl REPL (Section 7.7).
#
#   scripts/test_xrepl.sh                  run every manifests/p07_tests/*.repl
#   scripts/test_xrepl.sh type inspect     run only these tests (names without .repl)
#   scripts/test_xrepl.sh --update [names] rewrite the .expected files from the current output
#   scripts/test_xrepl.sh --dir DIR ...    use another test directory (default manifests/p07_tests)
#
# Without test names the run also includes the two pseudo-terminal checks of Tab completion.
#
# A test is NAME.repl (what a user types) and NAME.expected (the transcript scripts/session.py
# prints when it drives build/bin/p07_xrepl with those inputs). XREPL_DETERMINISTIC=1 turns the
# timings of %time into "<t>"; session.py masks addresses. Exit status 1 if any test fails.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/manifests/p07_tests"
UPDATE=0
NAMES=()
RUN_TTY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --update) UPDATE=1 ;;
    --dir) DIR="$(cd "$2" && pwd)"; shift ;;
    *) NAMES+=("$1") ;;
  esac
  shift
done
if [[ ${#NAMES[@]} -eq 0 ]]; then
  RUN_TTY=1 # Tab completion is tested through a pseudo-terminal, see scripts/test_xrepl_tty.py
  for f in "$DIR"/*.repl; do NAMES+=("$(basename "$f" .repl)"); done
fi

mkdir -p "$ROOT/out"
pass=0; fail=0
for name in "${NAMES[@]}"; do
  input="$DIR/$name.repl"
  expected="$DIR/$name.expected"
  actual="$ROOT/out/p07_actual_$name.txt"
  XREPL_HISTORY="$ROOT/out/p07_test_history" XREPL_DETERMINISTIC=1 python3 "$ROOT/scripts/session.py" --repl "$ROOT/build/bin/p07_xrepl" --prompt xrepl \
    "$input" >"$actual" 2>&1
  if [[ $UPDATE -eq 1 ]]; then
    cp "$actual" "$expected"
    echo "updated  $name"
  elif diff -u "$expected" "$actual" >"$ROOT/out/p07_diff_$name.txt" 2>&1; then
    echo "PASS     $name"; pass=$((pass + 1))
  else
    echo "FAIL     $name"; fail=$((fail + 1))
    sed -e '1,2d' -e 's|^|    |' "$ROOT/out/p07_diff_$name.txt" | head -20 # drop the ---/+++ header: it has file names and dates
  fi
done
[[ $UPDATE -eq 1 ]] && exit 0
if [[ $RUN_TTY -eq 1 ]]; then
  python3 "$ROOT/scripts/test_xrepl_tty.py" "$ROOT/build/bin/p07_xrepl"
  tty_failed=$? # the script exits with the number of failed checks (of 2)
  pass=$((pass + 2 - tty_failed)); fail=$((fail + tty_failed))
fi
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
