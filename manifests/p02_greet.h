// Part 2.4 -- the interface of a tiny C library, libp02greet.dylib.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *greet_version(void);
int greet_count(void);
void greet(const char *who);
#ifdef __cplusplus
}
#endif
