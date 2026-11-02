# Learning progress

The curriculum was reorganized around the JIT C++ APIs on 2026-10-31. These new lesson IDs start unchecked; the previous checklist is preserved in `out/jit-rewrite-backup/before.tar.gz`. Mark a lesson after completing its changes and explaining the result. Browser progress is stored independently for this curriculum.

## Part 1 — Your First Embedded JIT

- [ ] 1.1 — The idea: compile C++ inside an application
- [ ] 1.2 — The components and their responsibilities
- [ ] 1.3 — How the API calls fit together
- [ ] 1.4 — Build a host that links Clang
- [ ] 1.5 — Create the compiler and interpreter
- [ ] 1.6 — Call the generated machine code
- [ ] 1.7 — Capture an expression result
- [ ] 1.8 — Report a failed input
- [ ] 1.9 — Keep ownership visible
- [ ] 1.10 — Parse command-line options with LLVM

## Part 2 — Incremental Compilation

- [ ] 2.1 — `Parse` versus `Execute`
- [ ] 2.2 — Inspecting a PTU's AST
- [ ] 2.3 — Inspecting a PTU's LLVM IR
- [ ] 2.4 — Choose language and optimization settings
- [ ] 2.5 — Give the engine headers
- [ ] 2.6 — Type-check without executing

## Part 3 — Connecting Host and JIT

- [ ] 3.1 — Calling JIT code from the host
- [ ] 3.2 — Sharing state with the host
- [ ] 3.3 — Injecting host data into JIT code
- [ ] 3.4 — Load a supporting library through the API
- [ ] 3.5 — Register a host address explicitly
- [ ] 3.6 — `Undo` from the API
- [ ] 3.7 — Initializers and exception boundaries

## Part 4 — Clang Compiler Services

- [ ] 4.1 — Errors and diagnostics
- [ ] 4.2 — The code-completion API
- [ ] 4.3 — Name lookup with `Sema`
- [ ] 4.4 — Walking types and record layouts
- [ ] 4.5 — Templates on demand
- [ ] 4.6 — Choose an observation point

## Part 5 — Values, Objects and Lifetimes

- [ ] 5.1 — Anatomy of `clang::Value`
- [ ] 5.2 — Builtins, kinds and `convertTo`
- [ ] 5.3 — Objects, ownership and destructors
- [ ] 5.4 — The printing API
- [ ] 5.5 — An object inspector
- [ ] 5.6 — Release results before their engine

## Part 6 — Advanced JIT Customization

- [ ] 6.1 — Subclassing `Interpreter` with an `ASTConsumer`
- [ ] 6.2 — Reflection-driven code injection
- [ ] 6.3 — A custom `LLJIT` builder
- [ ] 6.4 — Capturing object code
- [ ] 6.5 — Writing your own `IncrementalExecutor`
- [ ] 6.6 — Out-of-process execution from the API
- [ ] 6.7 — Eval-style programming: JIT code calls the interpreter
- [ ] 6.8 — Syntax-only interpreters
- [ ] 6.9 — Choose an extension boundary

## Part 7 — A Callable C++ Kernel

- [ ] 7.1 — Compile a function supplied as a file
- [ ] 7.2 — Check the entry signature before calling
- [ ] 7.3 — Separate compile cost from call cost
- [ ] 7.4 — Replace a kernel without dangling pointers
- [ ] 7.5 — Turn observations into acceptance cases
- [ ] 7.6 — Extend the host in one direction

