// Part 2.3 -- a "prelude" header: the includes and helpers you want in every session.
// Used with  --Xcc=-include --Xcc=manifests/p02_prelude.h
// or precompiled (see Section 2.3) and used with --Xcc=-include-pch.
#pragma once
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

// Print any container of printable elements: show({1, 2, 3}) -> [1, 2, 3]
template <typename C> void show(const C &c) {
  std::cout << '[';
  bool first = true;
  for (const auto &e : c) {
    std::cout << (first ? "" : ", ") << e;
    first = false;
  }
  std::cout << "]\n";
}

#ifdef __OPTIMIZE__
inline const char *lab_build = "optimized";
#else
inline const char *lab_build = "debug";
#endif
