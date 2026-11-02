#!/usr/bin/env bash
# Invalid C-string pointers must be reported rather than dereferenced directly.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p out
RESULT="$(mktemp "$ROOT/out/inspector-test.XXXXXX")"
trap 'rm -f "$RESULT"' EXIT
build/bin/p05_inspect manifests/p05_pointer_reads.repl pointers >"$RESULT" 2>&1
for pattern in 'text .*"readable"' 'invalid .*<unreadable>' 'nulltext .*null' \
               'boundary .*<unreadable>' 'terminated .*"O"'; do
  if ! rg -q "$pattern" "$RESULT"; then
    printf 'FAIL: inspector output missing %s\n' "$pattern"
    cat "$RESULT"
    exit 1
  fi
done
echo 'inspector tests: readable, invalid, null and page-boundary pointers passed'
