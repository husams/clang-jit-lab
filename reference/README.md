# Upstream reference sources (Clang 22.1.8)

Read-only copies of the parts of the LLVM project that implement `clang-repl`, so the lab can point at real code with `sed -n`. Every file is byte-identical to tag [`llvmorg-22.1.8`](https://github.com/llvm/llvm-project/tree/llvmorg-22.1.8/clang) of `llvm/llvm-project` (checked on 2026-10-24), which is the version Homebrew installs as LLVM 22.1.8. They are Apache-2.0 WITH LLVM-exception licensed, like the rest of LLVM.

Do not edit them. The public headers are not copied: read the installed ones in `$LLVM/include/clang/Interpreter/`.

| Path (under `clang-22.x/`) | Upstream | What it is |
|----------------------------|----------|------------|
| `tools/clang-repl/ClangRepl.cpp` | `clang/tools/clang-repl/` | the `clang-repl` executable: options, `%` commands, line editor, completion |
| `lib/Interpreter/Interpreter.cpp` | `clang/lib/Interpreter/` | `IncrementalCompilerBuilder`, `Interpreter`, the runtime prelude, `Parse`/`Execute`/`Undo` |
| `lib/Interpreter/IncrementalParser.{h,cpp}` | | one `Parse()` per input: buffers named `input_line_N`, a new `TranslationUnitDecl` per input, rollback |
| `lib/Interpreter/IncrementalAction.{h,cpp}` | | the long-lived frontend action, the value-printing AST consumer, `GenModule()` |
| `lib/Interpreter/InterpreterValuePrinter.cpp` | | value printing: synthesising `__clang_Interpreter_SetValue*` calls, formatting values |
| `lib/Interpreter/Value.cpp` | | `clang::Value` storage, reference counting, destructors |
| `lib/Interpreter/IncrementalExecutor.cpp` | | `IncrementalExecutorBuilder`: in-process and out-of-process `LLJIT` setup, ORC runtime lookup |
| `lib/Interpreter/OrcIncrementalExecutor.{h,cpp}` | | the default executor: one `ResourceTracker` per input |
| `lib/Interpreter/CodeCompletion.cpp` | | `ReplCodeCompleter` |
| `lib/Interpreter/InterpreterUtils.{h,cpp}` | | small Sema/AST helpers used by the value printer |
| `lib/Interpreter/DeviceOffload.*`, `Wasm.*` | | CUDA and WebAssembly support (not used on macOS) |
| `unittests/Interpreter/*.cpp` | `clang/unittests/Interpreter/` | the API's own tests: the best examples of library use |
| `test/Interpreter/*` | `clang/test/Interpreter/` | lit tests of the `clang-repl` binary |
| `docs/ClangRepl.rst` | `clang/docs/` | the upstream documentation page |

Other sources the lab draws on:

- Clang-Repl documentation: <https://clang.llvm.org/docs/ClangRepl.html>
- RFC: Handle Execution Results in clang-repl: <https://discourse.llvm.org/t/rfc-handle-execution-results-in-clang-repl/68493>
- GSoC 2024: Out-Of-Process Execution For Clang-Repl: <https://blog.llvm.org/posts/2024-10-23-out-of-process-execution-for-clang-repl/>
- Interactive C++ for Data Science (eval-style programming, Cling): <https://blog.llvm.org/posts/2020-12-21-interactive-cpp-for-data-science/>
- Cling transitions to LLVM's Clang-Repl: <https://root.cern/blog/cling-in-llvm/>
- CppInterOp (a reflection layer on top of clang-repl): <https://github.com/compiler-research/CppInterOp>
- xeus-cpp (a Jupyter kernel on top of clang-repl): <https://github.com/compiler-research/xeus-cpp>
