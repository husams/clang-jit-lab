#!/usr/bin/env bash
# Run every native API tool with Linux behavior assertions.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ "$(uname -s)" != Linux ]]; then
  echo "test_linux.sh requires Linux" >&2
  exit 2
fi
export LLVM="${LLVM:-$(llvm-config-22 --prefix)}"
export PATH="$LLVM/bin:$PATH"
ulimit -c 0
mkdir -p out/linux-tests
fail=0
total=0
run() {
  local name="$1"
  shift
  total=$((total+1))
  local result=0
  timeout 45 "$@" >"out/linux-tests/$name.log" 2>&1 || result=$?
  if [[ "$result" != 0 ]]; then
    printf 'FAIL %-26s exit=%s\n' "$name" "$result"
    tail -12 "out/linux-tests/$name.log"
    fail=$((fail+1))
    return
  fi
  local pattern='' diagnostics=0
  case "$name" in
    smoke) pattern='host call: smoke_twice\(21\) = 42' ;;
    engine) pattern='captured = 35' ;;
    starter-call) pattern='run\(6\) = 42' ;;
    starter-capture) pattern='captured = 35' ;;
    starter-errors) pattern='after error = 42' ;;
    hello) pattern='\(int\) 82' ;;
    errors) pattern='\(int\) 7' ;;
    parse) pattern='quad\(5\) = 20' ;;
    ast) pattern='Function add' ;;
    ir) pattern='__stmts__' ;;
    ir-optimized) pattern='counter' ;;
    platform) pattern='-resource-dir' ;;
    undo) pattern='\(int\) 2' ;;
    call) pattern='100000 calls, final value 5000150000' ;;
    complete) pattern='lab_total' ;;
    diagnostics) pattern='inputs=10 failed=4' ;;
    anatomy) pattern='K_Int' ;;
    kinds-table) pattern='K_Double' ;;
    kinds-natural) pattern='getDouble\(\) = 2.9' ;;
    kinds-wrong) pattern='a conversion' ;;
    kinds-pointer) pattern='after the host wrote 43' ;;
    lookup) pattern='counter' ;;
    qualified-lookup) pattern='unit' ;;
    value-lookup) pattern='1000' ;;
    declarations) pattern='FunctionTemplate' ;;
    type-layout) pattern='canonical' ;;
    record-layout) pattern='Inner' ;;
    template-layout) pattern='Pair' ;;
    inspector) pattern='hello' ;;
    templates-address) pattern='8999991' ;;
    templates-explicit) pattern='8999991' ;;
    templates-class) pattern='sizeof =' ;;
    consumer) pattern='undo: 6 inputs undone' ;;
    reflection) pattern='Person\{name=Ada' ;;
    jit-absolute) pattern='secret: 42' ;;
    jit-missing|jit-missing-trace) pattern='Symbols not found:.*host_secret' ;;
    jit-trace) pattern='symbols the main JITDylib' ;;
    jit-debugger) pattern='registered with debuggers.*yes' ;;
    objects) pattern='ELF object' ;;
    executor) pattern='static destructor ran' ;;
    oop-spawn) pattern='JIT pid == ExecutorPID: yes' ;;
    oop-connect) pattern='JIT pid == the executor we started: yes' ;;
    oop-crash) pattern='host: still running, executor reaped: yes' ;;
    eval) pattern='pow4\(3\) = 81' ;;
    deferred-eval) pattern='\(int\) 2' ;;
    checker) pattern='has IR: no' ;;
    kernel) pattern='run\(6\) = 42' ;;
    kernel-tests) pattern='10 cases passed' ;;
    library) pattern='library result = 42' ;;
    plugin) pattern='\(int\) 41' ;;
    pch-use) pattern='\[1, 2, 3\]' ;;
    xrepl) pattern='\(int\) 42' ;;
    ownership-*) pattern='~T' ;;
    printing-*) pattern='(pretty:|type:|isValid=0)' ;;
    sharing-jit2host) pattern='jit_total read from C\+\+ = 6' ;;
    sharing-extern) pattern='after the interpreter added 5' ;;
    sharing-cpp) pattern='host calls jit_sub.* = 5' ;;
    sharing-static) pattern='Symbols not found' ;;
    injection-config) pattern='host now sees: verbosity=7' ;;
    injection-callback) pattern='host_report.*answer = 42' ;;
    injection-array) pattern='host computes the same sum: 31' ;;
    injection-reverse) pattern='host read' ;;
  esac
  case "$name" in
    starter-errors|errors|parse|undo|diagnostics|consumer|checker|sharing-static|oop-spawn|oop-crash|jit-missing|jit-missing-trace) diagnostics=1 ;;
  esac
  if [[ -n "$pattern" ]] && ! rg -q -- "$pattern" "out/linux-tests/$name.log"; then
    printf 'FAIL %-26s missing result: %s\n' "$name" "$pattern"
    tail -12 "out/linux-tests/$name.log"
    fail=$((fail+1))
  elif [[ "$diagnostics" == 0 ]] && rg -q '(^|[[:space:]])error:|LLVM ERROR:|attach failed:' "out/linux-tests/$name.log"; then
    printf 'FAIL %-26s unexpected diagnostic\n' "$name"
    tail -12 "out/linux-tests/$name.log"
    fail=$((fail+1))
  else
    printf 'PASS %s\n' "$name"
  fi
}
run smoke build/bin/p00_smoke
run engine build/bin/p01_engine
run starter-call build/bin/p01_call
run starter-capture build/bin/p01_capture
run starter-errors build/bin/p01_errors
run hello build/bin/p04_hello
run errors build/bin/p04_errors
run parse build/bin/p04_parse
run ast build/bin/p04_ast manifests/p04_ast.repl 4 5
run ir build/bin/p04_ir manifests/p04_ir.repl
run ir-optimized build/bin/p04_ir manifests/p04_ir.repl -- -O2
run platform build/bin/p04_platform
run undo build/bin/p04_undo
run call build/bin/p04_call
run complete build/bin/p04_complete manifests/p04_complete.repl lab_ lab_point.
run diagnostics build/bin/p04_diag manifests/p04_diag.repl -- -Wall
run anatomy build/bin/p05_anatomy manifests/p05_anatomy.repl
run anatomy-reuse build/bin/p05_anatomy reuse
run kinds-table build/bin/p05_kinds table
run kinds-natural build/bin/p05_kinds natural
run kinds-wrong build/bin/p05_kinds wrong
run kinds-pointer build/bin/p05_kinds ptr
for mode in temp lvalue copies safe; do run ownership-$mode build/bin/p05_ownership "$mode"; done
for mode in apis capture pretty; do run printing-$mode build/bin/p05_printing "$mode" manifests/p05_printing.repl; done
for mode in extern cpp static jit2host; do run sharing-$mode build/bin/p05_shared "$mode"; done
for mode in config callback array reverse; do run injection-$mode build/bin/p05_inject "$mode"; done
run lookup build/bin/p05_lookup manifests/p05_lookup.repl lookup counter ratio over Point
run qualified-lookup build/bin/p05_lookup manifests/p05_lookup.repl qualified geo unit Box area
run value-lookup build/bin/p05_lookup manifests/p05_lookup.repl value counter ratio origin
run declarations build/bin/p05_lookup manifests/p05_lookup.repl decls
run type-layout build/bin/p05_layout manifests/p05_layout.repl types answer pid count text deduced
run record-layout build/bin/p05_layout manifests/p05_layout.repl layout Inner Layout Poly VD Bits U
run template-layout build/bin/p05_layout manifests/p05_layout.repl template Pair
run inspector build/bin/p05_inspect manifests/p05_inspect.repl scene small big
for mode in address explicit class; do run templates-$mode build/bin/p05_templates manifests/p05_templates.repl "$mode" int double; done
run consumer build/bin/p06_consumer manifests/p06_consumer.repl
run reflection build/bin/p06_inject manifests/p06_reflect.repl
run jit-absolute build/bin/p06_jit --abs manifests/p06_hostcall.repl
run jit-missing build/bin/p06_jit manifests/p06_hostcall.repl
run jit-missing-trace build/bin/p06_jit --trace manifests/p06_hostcall.repl
run jit-trace build/bin/p06_jit --abs --trace manifests/p06_hostcall.repl
run jit-debugger build/bin/p06_jit --abs --debugger manifests/p06_hostcall.repl -- -g
run objects build/bin/p06_objects out/linux-objects manifests/p06_objs.repl
run executor build/bin/p06_executor manifests/p06_protocol.repl
run oop-spawn build/bin/p06_oop spawn
run oop-connect build/bin/p06_oop connect
run oop-crash build/bin/p06_oop crash
run eval build/bin/p06_eval manifests/p06_eval.repl
run deferred-eval build/bin/p06_eval manifests/p06_defer.repl
run checker build/bin/p06_check manifests/p06_snippets.repl
run kernel build/bin/p07_kernel manifests/p07_kernel.cpp 0 6 -3
run kernel-tests scripts/test_kernel.sh
run inspector-pointer-tests scripts/test_inspector.sh
run library-build "$LLVM/bin/clang++" -fPIC -shared manifests/p03_library.cpp -o out/p03_library.so
run library build/bin/p03_library out/p03_library.so
run plugin-build "$LLVM/bin/clang++" -std=c++17 -fno-rtti -fPIC -shared -I"$LLVM/include" manifests/p02_plugin.cpp -L"$LLVM/lib" -lclang-cpp -lLLVM -o out/p06_printdecls.so
run plugin build/bin/p04_hello 'int a = 41;' 'a' -- -Xclang -load -Xclang out/p06_printdecls.so -Xclang -add-plugin -Xclang print-decls
run pch-build "$LLVM/bin/clang++" -std=c++20 -x c++-header -Xclang -fincremental-extensions -Xclang -emit-pch manifests/p02_prelude.h -o out/p02_prelude.pch
run pch-use build/bin/p04_hello 'show(std::vector<int>{1, 2, 3});' -- -std=c++20 -include-pch out/p02_prelude.pch
run xrepl bash -c "printf '%s\n' 'int n = 6;' 'n * 7' '%quit' | build/bin/p07_xrepl"
printf 'Linux API tests: %d cases, %d failed\n' "$total" "$fail"
exit "$fail"
