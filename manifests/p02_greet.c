// Part 2.4 -- a tiny C library to load with %lib.
// Build:  $LLVM/bin/clang -shared -fPIC manifests/p02_greet.c -o out/libp02greet.dylib
#include <stdio.h>
#include "p02_greet.h"

static int count;

const char *greet_version(void) { return "greet 1.0"; }
int greet_count(void) { return count; }
void greet(const char *who) {
  ++count;
  printf("hello, %s (greeting #%d)\n", who, count);
}
