// Part 3.3 -- top-level statements in an ordinary file, compiled with
//   $LLVM/bin/clang -cc1 -fincremental-extensions -emit-llvm -o - manifests/p03_stmts.cpp
// All top-level statements of one translation unit are collected, in order,
// into one internal function that the module's static initializer calls.
extern "C" int printf(const char *, ...);
int counter = 1;
++counter;
printf("counter = %d\n", counter);
int twice(int v) { return 2 * v; }
counter = twice(counter);
