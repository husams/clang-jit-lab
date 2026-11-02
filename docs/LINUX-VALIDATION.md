# Linux validation — 2026-10-31

The documented Ubuntu installation and every C++ tool were tested with LLVM/Clang 22.1.8 in Ubuntu 24.04.5 amd64 userspace on an x86_64 Linux 5.14 host. The environment was an isolated filesystem on linux-dev, not a separate Ubuntu kernel. Installation ran with administrative privileges; the final full check also passed as an ordinary user (UID 1000).

| Check | Result |
|---|---|
| Published Ubuntu installation commands | Passed; compiler and development packages installed from apt.llvm.org. |
| CMake/Ninja builds | All 33 native C++ tools built. |
| Linux API regression suite | 70 cases passed, including success and deliberate-diagnostic paths. |
| Kernel interface acceptance | 10 cases passed. |
| Inspector pointer regression | Readable, null, invalid and protected-page boundaries passed. |
| Connected executor shutdown | Passed, including three extra bounded repetitions. |
| Ordinary-user full check | Passed, including native tests, links and HTML generation. |
| macOS regression check | Passed: 117 recorded commands, 11 optional REPL tests and the native acceptance checks. |

The Linux suite exercises incremental parsing and execution, AST/IR inspection, diagnostics, completion, symbol lookup, shared host state, pointer/callback injection, Value conversions and ownership, record layouts, templates, generated printers, custom JIT/executor hooks, object capture, separate executors, nested/deferred evaluation, syntax-only checking, shared libraries, frontend plugins, PCH and the callable kernel. Deliberately undefined-behavior demonstrations are not promoted to success-path acceptance cases.

Three executable problems were reproduced and fixed:

- The inspector included Mach headers unconditionally. It now uses `vm_read_overwrite` on macOS and `process_vm_readv` against itself on Linux; a guarded-page regression checks that unreadable C strings are reported without a crash.
- Clang’s runtime-path callback missed Ubuntu’s installed `lib/linux/liborc_rt-<arch>.a` archive. The helper retains normal discovery and adds the distribution-layout fallback using the actual compilation target.
- LLVM 22’s TCP file-descriptor transport could hang while the interpreter was destroyed. The Linux example composes the public transport interface and shuts down the socket before closing it, waking its blocked reader. A failed connection now returns a failing exit status.

The full Linux check also exposed a documentation dependency on a personal wiki outside the lab; that link was removed so a standalone copy validates correctly. The first-engine lesson now contains the complete compilable starter, including its headers and argument setup.

Run the same checks after following Part 1’s installation instructions:

```bash
scripts/check.sh --quick
```

For already built tools, run the Linux behavior suite directly:

```bash
scripts/test_linux.sh
```

Raw installation output, per-case logs, the executor backtrace and source checksums are retained under `out/linux-validation/`. The temporary Ubuntu filesystem and mounts were removed after saving the results. The test environment used the published Ubuntu base image and LLVM package repository; other Linux distributions, architectures and LLVM versions were not validated by this run.

Sources: [Ubuntu base images](https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/), [official LLVM packages](https://apt.llvm.org/), [Linux process_vm_readv API](https://man7.org/linux/man-pages/man2/process_vm_readv.2.html), [LLVM 22 FD transport implementation](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/lib/ExecutionEngine/Orc/Shared/SimpleRemoteEPCUtils.cpp).

## Complete-program follow-up

The expanded lessons were tested again on both platforms with LLVM 22.1.8. Every C++ fence now contains a complete named source file or header, with its lab dependencies and Linux/macOS build/run commands. The synchronization check covers 28 lessons, 31 displayed files and 30 distinct source/header files. `python3 scripts/test_complete_examples.py` extracts the actual Markdown code into a fresh source tree, builds every tool and runs the printed commands for the current platform; all 28 lesson command blocks passed on Linux and all 28 passed on macOS.

The new `p01_call`, `p01_capture` and `p01_errors` hosts bring the tool count to 36. All 36 built in the Linux validation tree, and the API suite passed 73 cases as ordinary UID 1000. Kernel acceptance and inspector boundary checks passed; macOS also passed 11 optional REPL regressions and 116 recorded command comparisons. The optional undefined-behavior destruction experiment is retained as a recorded example but no longer asserts an allocator-dependent exit status. Markdown links and 913 generated-site checks passed, including exact code-block parity between Markdown and HTML.

The follow-up evidence is in `out/complete-examples/`: `macos.log` and `linux-run.log` record extracted-program execution, `macos-regression.log` records the macOS full check, and `linux-site.log` records the final Linux link/HTML checks. The Linux copy initially omitted the linked `CLAUDE.md` authoring file; transferring that repository file resolved the link check without changing the lab. Individual Linux lesson and API logs are saved in `linux-evidence.tar.gz`. The temporary Ubuntu installation used the published Linux commands again, leaving the host's packages untouched.

## Independent API programs

The current lesson programs live under `examples/` and require only public LLVM/Clang, standard C++ and platform system headers. Their compiler setup and sample inputs are visible in the file. They use no lab helper header, generated platform header, framework macro or required fixture directory. The two-file library and kernel exercises show both source files completely.

The printed commands compile with `clang++` and `llvm-config --cxxflags`, link `clang-cpp` and `LLVM`, and pass header-search options explicitly to the embedded compiler. Linux uses the installed resource directory and `/usr` GCC toolchain; macOS uses the resource directory, libc++ headers and SDK. The checker extracts only `examples/`, so the build cannot rely on the reference tools or their CMake configuration.

All 34 lesson command blocks were compiled and run on Ubuntu 24.04.5 amd64 and macOS with LLVM 22.1.8, using targeted reruns after corrections. The Linux execution user was UID 1000. The current edition contains 35 distinct source files in 36 displayed C++ blocks. The validation caught and corrected library path conversion and symbol consistency, preserved syntax-only mode when explicit compiler arguments are present, and guarded invalid presumed source locations. All Markdown links and 913 generated-site checks passed, including exact Markdown/HTML code parity.

Evidence is under `out/standalone-examples/`: `validation.json` records the final result for every lesson on each platform; the macOS and Linux logs retain initial runs and corrections; `linux-evidence.tar.gz` retains the individual Linux command logs and source checksums. The pre-change source snapshot is `before.tar.gz` in the same directory. The temporary Linux filesystem is removed after saving the evidence; host packages are unchanged.

## Direct API and error handling

The current programs call compiler construction, interpreter creation, parsing, execution and lookup directly where each operation occurs. `Check`, `ExitOnError`, compiler/interpreter factories and execution aliases are removed. Each fallible operation exposes its `llvm::Expected` or `llvm::Error` handling. Argument parsing and source inputs are ordinary loops and data; formatting and inspection shortcuts no longer hide the SDK calls.

All 34 command blocks passed again on Ubuntu 24.04.5 amd64 as UID 1000 and on macOS, using LLVM 22.1.8. Eight additional cases passed on each platform: invalid C++ standard, syntax error, incorrect signature, missing definition, accepted integer alias, unresolved symbol, absent source file and invalid compiler setup for a remote connection. The remote failure test initially exposed a Linux teardown crash; validating the compiler before constructing the remote transport fixed the tested path, and the child is reaped on failure. The normal spawn/connection tests passed after that change.

`out/direct-api/validation.json` records each lesson’s latest passing result and the error cases. Individual Linux logs and source checksums are retained in `out/direct-api/linux-evidence.tar.gz`; macOS logs include targeted correction runs. Formatting, Markdown links, 913 generated-site checks and all 116 preserved macOS reference transcripts passed. The temporary Ubuntu test filesystem is removed after saving the evidence, without changing the host’s packages.
