# Authoring the Clang JIT lab

Teach people to use Clang's C++ APIs in their own host applications. The first complete example is examples/p01_engine.cpp; the final host is examples/p07_kernel.cpp. A prompt frontend is optional material at the end.

Write a lesson as connected prose: introduce one concrete problem, explain the API while showing its code, ask the reader to predict a result, run the example, then change one thing and interpret the difference. Put observations beside the code that produces them. Never add generic Why, What to Do, Verify or Expected headings, or turn every paragraph into a collapsible box. Technical subheadings should name actual concepts.

Keep learner-facing prose about the concept, API behavior and exercise. Omit assurances about example completeness, public headers, helper removal and how the lab was authored or validated. Do not repeat command-line reminders in every section. The example generator emits filenames, source and short platform labels; explanations belong in the lesson's natural flow. Keep authoring and validation procedures in this guide.

Use Section 1.7 as the model for introductions: begin with a concrete situation, explain the concept and relevant API in ordinary language, then connect it to what the reader will examine or change. Give each introduction its own explanation; avoid repeated learning-objective formulas, file-location announcements and separate purpose/task labels. Introduce the idea before the API inventory or implementation details.

Continue that explanation through the lesson body. Connect the actual calls to the state they read or change, explain the relevant ownership and error branches, and interpret observations beside their runs. Avoid retelling the introduction or replacing explanation with numbered transcript summaries. Distinguish the standalone program's named options from the reference tools' fixture commands when an exercise uses one of them. Keep folded hints specific to the question, and retain version-dependent limitations beside the affected API.

Every numbered lesson needs hands-on work: editing a source string, changing a compiler option, inspecting a declaration, implementing a callback or testing a failure. Replaying a transcript alone is insufficient. Use folded Obsidian callouts for hints and explanations so readers can predict before revealing them.

## Keeping the learning path consistent

Use `# Part N — Title` and `## Section N.M — Title`. Update headings, docs/README.md section lists and docs/PROGRESS.md together. Parts have previous/next navigation at top and bottom; scripts/check_links.py checks the agreements. Historical filenames stay in place to preserve links; headings describe the current JIT curriculum.

Keep learning progress separate from implementation status. New lesson IDs start unchecked and HTML completion uses clangjit.v3.done; navigation preferences remain under clangjit.v2.*. docs/REVISION.md records authoring work. The former checklist is saved in out/jit-rewrite-backup/before.tar.gz.

## Building examples

Create tools/pNN_name/main.cpp, then run scripts/build.sh pNN_name. CMake discovers it and links clang-cpp and LLVM. It configures include directories, C++20, runtime paths, RTTI compatibility and platform definitions.

| Helper in tools/common/repllab.h | Responsibility |
|---|---|
| `init()` | Register native target, printer and parser; keep output ordered. |
| `platformArgs()` | Resource headers, Homebrew libc++ and macOS SDK paths. |
| `makeCompiler(flags)` | Configure IncrementalCompilerBuilder and create a compiler. |
| `makeInterpreter(flags, builder)` | Transfer the compiler into an interpreter. |
| `run(I, source, value)` | Submit input and consume/report errors. |
| `replInputs(text)` | Split a fixture into ordered API inputs. |
| `maskAddrs(text)` | Normalize addresses in recorded output. |

Show the APIs being taught directly; do not hide all compiler construction behind helpers. Keep argument strings alive until CreateCpp completes, use exact ABI types at calls, and make ownership explicit around undo/session destruction. Existing .repl fixtures used by native tools are ordered API inputs; they do not require teaching shell commands.

## Recording real runs

A `text expected` fence is a machine-readable fixture marker, not a lesson heading. It records the output of the preceding bash fence. HTML labels it recorded output. Leave a new recorded fence empty and fill it by running the command:

```bash
python3 scripts/doccheck.py --fill docs/part_1_shell_basics.md
python3 scripts/doccheck.py docs/part_1_shell_basics.md
```

Each recorded command runs from the lab root in a fresh shell; LLVM defaults to /opt/homebrew/opt/llvm. Keep build prerequisites in earlier unrecorded blocks. scripts/check.sh must prepare files needed by recorded commands. Manually validate changed unrecorded commands too. Never invent successful output or use a failure transcript as proof that the success path works.

Keep output deterministic: normalize addresses, paths and unordered symbols; do not record raw timings or PIDs. Keep dumps focused. Read every filled block and explain its actual result.

## Diagrams and HTML

Use Graphviz dot fences, semantic classes such as api, data, entry and external, and site color tokens. Do not hard-code diagram colors/fonts. scripts/build_site.py renders interactive SVG controls.

Dark mode stays active; navigation collapses to a desktop rail or mobile drawer. Preserve keyboard navigation, filter, copy buttons, folded hints and completion controls. Headings stay in the ordinary reading flow. The browser does not execute C++.

## Validation

```bash
scripts/build.sh
scripts/test_kernel.sh
python3 scripts/doccheck.py docs/part_*.md
python3 scripts/check_links.py
python3 scripts/build_site.py
python3 scripts/check_site.py
```

scripts/check.sh --quick combines checks and optional REPL regressions. Inspect desktop and narrow layouts in a real browser; exercise sidebar persistence, hints, search, copy and progress. Publication requires a separate request.

Pin claims to installed LLVM 22.1.8 headers and matching upstream code. Preserve verified limits: Value move construction is broken in this version; Undo cannot reverse arbitrary effects; completion uses a separate compiler context; a remote executor has a separate address space; remote execution alone is not a security sandbox.

Start the learning path with the mental model, component responsibilities and API sequence. Keep Linux and macOS setup distinct, preserve the host standard-library ABI, and label platform-specific recorded output. The Linux check runner executes scripts/test_linux.sh across every native API tool, including the inspector and socket-shutdown regressions, and skips comparisons with macOS golden transcripts.
# Complete C++ examples

Keep each lesson's C++ fences as complete named files. Edit the corresponding standalone source under `examples/`, then run `python3 scripts/complete_examples.py --write` to refresh the Markdown copies and both platform command blocks. The registry in that script identifies each lesson's files, target and invocation. Retain the marked example boundaries; the HTML generator hides them.

`python3 scripts/complete_examples.py` checks for drift and unregistered C++ fragments. `python3 scripts/test_complete_examples.py` extracts the actual Markdown fences into a fresh directory containing only `examples/`, compiles the standalone programs, executes every printed command for the current platform and checks the results. Run it on both Linux and macOS after changing example code or build commands. Localhost access is needed by the executor connection exercise. `scripts/check.sh --quick` also checks synchronization, the native regressions, Markdown links and exact HTML code-block parity. Failure demonstrations with undefined behavior must not assert a fixed output or exit status.


Lesson programs must include only public Clang/LLVM, standard C++ and platform system headers. Define application functions and sample inputs in the file; never require a lab helper header, generated configuration or implicit compiler flags. Show direct compiler/linker commands and the embedded compiler’s header-search arguments. The independent-source check rejects framework includes and macros. Internal reference tools under `tools/` remain separate from the sources used by the lessons.

Show the actual compiler builder, interpreter creation, parsing, execution, lookup and `llvm::Expected`/`llvm::Error` handling where each operation happens. Do not add `makeCompiler`, `makeInterpreter`, `run`, `eval`, `Check`, `ExitOnError`, or equivalent convenience wrappers. Input data can be a literal container. Keep only functions that implement the demonstrated feature itself, such as required SDK callbacks or a recursive printer; their bodies must also call the APIs directly.

Preserve the initial `p01_engine.cpp` program's behavior, argument handling and source strings; comments may explain its existing API calls. Every other host uses `llvm::cl` directly with typed named options and a positional compiler-flag tail after `--`. Keep the option declarations and `ParseCommandLineOptions` visible; do not introduce a shared parsing helper. The kernel and shared-library input translation units have no command-line interface. The HTML highlighter and its C++ grammar are bundled in `scripts/site_assets/vendor/`; keep the local assets, license and contrasting syntax tokens when regenerating the site.

Write C++ submitted to the JIT as readable multiline raw strings, with ordinary quotes and formatted declarations and function bodies. Keep each incremental input separate and preserve its final-semicolon behavior. Use a different raw-string delimiter for nested JIT source. Dynamic code generation may concatenate short raw fragments around runtime names or addresses; show larger generated bodies as multiline source. Retain the initial `p01_engine.cpp` source-string format.

Put short teaching comments beside the important API calls inside each standalone source. Explain the state a call reads or changes, ownership transfers, argument lifetimes, result capture and ABI assumptions. Add detail for the demonstrated compiler or JIT feature rather than narrating every repeated call. Remove comments left behind by deleted helpers, and keep comments outside submitted source strings when only annotating host behavior. Regenerate both editions from the sources after comment edits, and verify that comments have not changed executable tokens, JIT input text or build commands.
