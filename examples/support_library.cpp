// Export a C-linkage definition; the JIT still needs a matching declaration to compile a call.
extern "C" int triple(int x) { return 3 * x; }
