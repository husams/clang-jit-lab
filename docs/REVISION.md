# JIT curriculum revision

- [x] Replace shell lessons with an API-first learning path.
- [x] Add editable first-engine and callable-kernel examples.
- [x] Connect explanations, experiments and exercises without formulaic headings.
- [x] Regenerate the dark HTML edition and retain collapsible navigation.
- [x] Check builds, recorded outputs, links and browser interactions.

The previous Markdown and site sources are saved in `out/jit-rewrite-backup/before.tar.gz`.
Learner completion is recorded separately in `PROGRESS.md`.

Validation on 2026-10-31 with Homebrew LLVM 22.1.8: all tools built; 10 kernel acceptance cases and 11 optional REPL regressions passed; 117 recorded commands matched; 867 HTML checks and all Markdown links passed. The PCH command was also exercised separately. Browser checks covered dark mode with a light OS preference, 320-pixel mobile and 1440-pixel desktop layouts, sidebar collapse/persistence, hints, filter, completion persistence and copy controls; no console errors were reported.

## Follow-up corrections

- [x] Make the sidebar an explicit, keyboard-accessible scroll area.
- [x] Put concepts, components and API responsibilities before build commands.
- [x] Add Linux setup and platform-specific library/plugin instructions.
- [x] Detect Linux LLVM/include paths and export host callbacks.
- [x] Validate consecutive numbering and restore results when search clears.
- [x] Rebuild and validate the revised editions and native tools.

Linux execution is unverified in this editing session; no local Linux VM is running.

Follow-up validation: 49 numbered lessons in consecutive order; 117 recorded commands matched; 10 kernel cases and 11 optional REPL regressions passed; 906 generated-site checks passed. The served localhost edition was checked for the introductory lesson at s1-1, the complete Parts 1–7 list, scrolling in both directions and restoration of all lessons after clearing search. Linux setup was checked against official LLVM package documentation; execution remains unverified without a running Linux environment.

## Executed Linux validation

Ubuntu 24.04.5 amd64 userspace with LLVM 22.1.8: all 33 tools built and 70 API cases passed, including a full ordinary-user run. Fixed the inspector’s Mach-only dependency, Ubuntu ORC archive discovery, a Linux TCP shutdown hang and false success on failed attach. Removed the external-wiki link dependency and included the full starter with headers. Kernel/inspector acceptance, three repeat connection teardowns, macOS regressions, links and generated HTML passed. See LINUX-VALIDATION.md; earlier unverified statements above describe the earlier stage of the revision.

## Complete runnable examples

- [x] Replace all abbreviated C++ fences with complete named source files.
- [x] Include required headers, supporting files and Linux/macOS build/run commands beside each example.
- [x] Add focused first-call, captured-result and handled-error hosts.
- [x] Check source/Markdown synchronization and exact HTML code parity.
- [x] Compile Markdown-extracted sources and execute all 28 lesson command blocks on both platforms.
- [x] Build all 36 native tools on macOS and Linux; pass the 73-case Linux API suite, 10 kernel cases, inspector checks and 11 macOS REPL regressions.

The extracted programs include 30 distinct source/header files displayed in 31 C++ blocks. Both platform runs used LLVM 22.1.8; Linux ran as UID 1000 in the temporary Ubuntu 24.04.5 filesystem. The macOS reference check passed 116 command comparisons. The optional undefined-behavior destructor experiment now labels its output as one recorded run instead of requiring a particular exit status. All Markdown links and 913 HTML checks passed. Browser inspection confirmed complete source, both platform commands, dark mode and scrolling in both directions. Evidence is under `out/complete-examples/`; the pre-change source backup is `out/complete-examples/before.tar.gz`.

## Independent API examples

- [x] Replace lesson code that depended on shared lab helpers with standalone sources under `examples/`.
- [x] Show public Clang/LLVM and standard/system headers, complete setup functions and embedded sample inputs.
- [x] Remove generated platform configuration, framework macros, private headers and required input fixtures from lesson programs.
- [x] Show direct `clang++` builds using the installed LLVM compiler flags, libraries and explicit embedded-compiler header paths.
- [x] Add complete standalone AST, IR, compiler-option, symbol-call, undo, layout and diagnostic examples.
- [x] Compile and run all 34 lesson command blocks on macOS and Linux with LLVM 22.1.8.

The independent edition has 35 distinct source files displayed in 36 C++ blocks. Extraction copies only `examples/`; the programs compile without `tools/`, build scripts, CMake configuration or fixture directories. The library and kernel exercises include their separate source files in full. Targeted reruns validated corrections to library argument/symbol handling, syntax-only configuration and source-location checks. All 34 command blocks passed on each platform across the recorded runs; `out/standalone-examples/validation.json` maps each lesson to its latest result. Linux ran as ordinary UID 1000. Markdown links, exact HTML code parity and 913 site checks passed. Browser inspection found no lab dependencies in displayed C++ and confirmed dark mode and scrolling in both directions.

## Direct API calls

- [x] Remove compiler/interpreter factories, execution aliases, argument-parser helpers and input-provider functions from lesson code.
- [x] Replace `Check` and `ExitOnError` with explicit `llvm::Expected` and `llvm::Error` branches at each operation.
- [x] Inline value formatting, kind switches, lookup, layout inspection, object handling and other presentation shortcuts.
- [x] Retain only implemented features and required SDK callbacks, with their own API calls shown directly.
- [x] Update both editions and reject wrapper regressions during source synchronization.
- [x] Validate all 34 lesson commands and eight additional error paths on Linux and macOS with LLVM 22.1.8.

The error cases cover an invalid C++ standard, syntax failure, a wrong entry signature, a missing definition, a valid type alias, an unresolved symbol, a missing file and failed remote compiler setup. That last case exposed a Linux teardown failure when the transport was opened before compiler validation; the connection example now validates the frontend first and reaps the child on failure. The final success and failure paths passed on both platforms. Formatting, Markdown links, 913 HTML checks and 116 preserved reference transcripts passed. Browser inspection found no setup/execution/result-check wrappers in any of the 36 displayed C++ blocks. Evidence and per-lesson results are under `out/direct-api/`; the previous sources are saved in its `before.tar.gz` snapshot.

## Command-line parsing in the introductory part — 2026-11-01

Added Section 1.10 and the independent `examples/p01_options.cpp` program while preserving `examples/p01_engine.cpp` byte for byte. The lesson separates LLVM host-option parsing from Clang compiler-flag parsing, demonstrates generated help, typed defaults, aliases, command-specific options, `--` forwarding, and `run`/`compile` subcommands. Exercises add a repeat option and an inspect command. Both editions include complete source, direct API calls, explicit error handling and Linux/macOS build commands; existing completion IDs stay in place and the new lesson starts unchecked.

The exact printed commands and eight additional help/error cases passed on macOS and Ubuntu 24.04.5 x86_64, using LLVM 22.1.8. Linux ran as UID 1000 in a temporary filesystem; host packages were unchanged. Both extension exercises were compiled and executed on macOS. Source synchronization, formatting, Markdown links, the existing Part 1 transcript and 925 generated-site checks passed. The local browser displays Section 1.10. Logs are under `out/command-line/`.

## LLVM option parsing throughout and local syntax colors — 2026-11-01

All 33 hosts after the initial example now call `llvm::cl::ParseCommandLineOptions` directly. Typed options cover mode selection, input numbers, completion limits, repeated source strings/names/types, output paths and boolean switches; compiler arguments remain a positional tail after `--`. No shared parser helper was added. The initial `p01_engine.cpp` source and its complete Markdown example are byte-for-byte unchanged. The separate kernel and shared-library input files do not have command-line interfaces. Both editions show the updated complete sources and Linux/macOS commands. Learning progress was not changed.

The HTML edition bundles highlight.js 11.9.0 and its C++/Bash grammars locally, with its BSD license. Keywords, types, strings, numbers, functions, preprocessing and comments have distinct dark-theme colors. Browser inspection confirmed highlighting in all 37 C++ blocks across seven parts, eight rendered colors including ordinary text, no external scripts, a minimum measured contrast of 8.15:1, dark mode and scrollable navigation. The site checker now requires the local highlighter before app.js.

All 35 Markdown-extracted lesson command blocks passed on macOS and Ubuntu 24.04.5 x86_64 with LLVM 22.1.8, including generated help and rejection of unknown options for every parsed host. Ten additional parser cases passed on each platform: typed-value errors, required arguments, repeated/quoted source input, qualified names and IR output. The 116 reference transcript commands, source synchronization, formatting, Markdown links and 935 generated-site checks passed. Linux tests ran as UID 1000 in a temporary filesystem; host packages were unchanged. Evidence and the review patch are under `out/parser-migration/`.

## Concept-focused prose and readable result capture — 2026-11-01

Removed recurring completeness/header assurances, parser reminders, redundant part introductions and authoring/validation commentary from the learner-facing text. Platform labels are now brief; the example generator no longer reintroduces descriptive boilerplate. Repeated source-status comments were removed while preserving the initial host source.

Section 1.7 now introduces the problem of bringing an expression's answer back into the host, explains the Value pointer and result conversion, and presents the JIT function in a multiline raw string. Both editions were regenerated. The result-capture program compiled and ran successfully, command extraction remains valid for all 70 platform blocks, links and 935 site checks passed, and browser inspection confirmed the new introduction, readable source, highlighting and absence of the repeated boilerplate.

## Readable multiline JIT source throughout — 2026-11-01

Replaced embedded C++ source literals with formatted multiline raw strings throughout the remaining hosts, preserving the initial `p01_engine.cpp`. Each incremental input stays separate, including all 154 entries across the 17 ordered source lists. Expressions retain their final-semicolon behavior. Reflection and nested evaluation now show larger generated bodies as multiline source with readable quotes and distinct nested delimiters. The first-line input preview ignores surrounding whitespace introduced by the raw-string layout.

Both editions were synchronized. HTML also applies C++ syntax colors inside multiline source literals, including nested source generation; all 37 displayed C++ blocks remain highlighted and browser text hashes match the Markdown source. Dark mode and collapsible navigation remain unchanged. All 35 printed lesson command blocks passed on both macOS and Ubuntu 24.04.5 x86_64 using LLVM 22.1.8. Targeted reruns cover the final reflection, nested-evaluation and input-preview refinements. `out/raw-strings/validation.json` maps every lesson to a passing run whose source files match the final edition. Formatting, links and all 935 site checks passed. Linux ran as UID 1000 in a temporary filesystem; host packages were unchanged.

## Natural introductions across the learning path — 2026-11-01

Rewrote the other 49 section introductions using Section 1.7's approach: a concrete situation, a plain explanation of the concept and its API, and a connection to the reader's experiment. Introductions now precede component inventories and comparison tables, and redundant file-location announcements were removed from the opening prose. Section 1.7 itself was retained.

All 36 C++ source files and every lesson code/command fence are unchanged. Markdown synchronization, links and 935 generated-site checks passed. Browser inspection confirmed introductory prose in all 50 sections across seven parts and highlighting in all 37 C++ blocks. Section IDs, learning progress, dark mode and navigation were preserved. Authoring guidance now records this introduction style; evidence is under `out/introductions/`.

## Connected explanations throughout lesson bodies — 2026-11-01

Rewrote the bodies of all 50 sections, retaining their introductions. Explanations now connect individual API calls to compiler state, executable code, ownership, diagnostics and results. Replaced terse transcript inventories and generic answer labels with connected observations and specific folded questions. Exercises retain concrete predictions and modifications. The extension comparison now helps choose a hook rather than repeating each preceding transcript. Version-specific capture, removal, re-entry and remote-cleanup limitations remain beside the relevant code.

All 36 C++ sources and every code, command, output and diagram fence are byte-for-byte unchanged. Source synchronization, Markdown links and all 935 site checks passed after regenerating HTML. Browser inspection covered all seven parts and confirmed 50 populated sections and highlighting in all 37 C++ blocks, with dark mode and the scrollable, collapsible sidebar retained. No native code was changed or rerun for this prose revision, and learning completion was not modified. The backup and preservation evidence are under `out/lesson-prose/`.

## API explanations inside the C++ examples — 2026-11-01

Added comments throughout all 36 standalone source files, explaining compiler configuration, checked errors, interpreter ownership, parsing versus execution, symbol lookup, ABI assumptions and result lifetimes. Feature-specific comments cover diagnostics, completion contexts, Sema lookup, type/layout inspection, template instantiation, object capture, generated-source scheduling, custom executors and remote runtime setup. Removed comments left behind by deleted helpers. The initial program keeps its argument handling, executable code and original source-string format; it now has explanatory comments too.

Regenerated the Markdown examples and HTML. Clang's raw lexer confirms identical non-comment executable tokens and literal contents in all 36 sources. All seven lesson files retain their surrounding prose, platform commands, recorded outputs and diagrams; learning progress is unchanged. Source synchronization, Markdown links and all 935 site checks passed. The printed macOS commands for initial construction, completion, custom execution and the validated kernel compiled and ran successfully. Linux was not rerun for these comment-only edits. Browser inspection confirmed highlighted comments in all 37 C++ blocks across seven parts, with dark mode and scrollable, collapsible navigation retained. Backup, annotation and validation evidence are under `out/api-comments/`; native run logs are in `out/complete-examples/run-epdclw0k/`.
