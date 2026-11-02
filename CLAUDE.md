# Clang JIT Engine Lab — Agent Guide

Teach Clang's incremental compiler and LLVM JIT through their C++ APIs. The learner builds a host, compiles source, calls generated functions and extends the compiler/executor. The clang-repl shell and optional p07_xrepl frontend are supporting examples.

Use one short sentence in ordinary replies; expand when presenting a requested lesson or details.

Present one numbered lesson at a time using connected explanation, a prediction, a small run and a code change. Let the learner answer before revealing folded hints and explanations. Never divide lessons into Why, What to Do, Verify or Expected sections. Explain results beside the code that produces them. Use technical subheadings only when useful.

Check docs/PROGRESS.md when asked to resume learning. Mark completion only after the learner completes the lesson; successful authoring checks do not mean learning is complete. HTML completion uses clangjit.v3.done; prior JIT lesson completion is migrated to the shifted Part 1 numbers. Navigation preferences remain under clangjit.v2.*.

The lab has Linux (Ubuntu/Debian) and macOS setup for LLVM 22. Reference transcripts were validated on macOS with Homebrew LLVM 22.1.8; all 33 tools and 70 behavior cases were validated in Ubuntu 24.04.5 x86_64 userspace with apt.llvm.org LLVM 22.1.8. Use installed headers and matching sources in reference/clang-22.x/ for API signatures. Do not assume compatibility after an upgrade.

The curriculum progresses through first engine construction, incremental compilation, host integration, Clang compiler services, Values and lifetimes, advanced JIT customization, and a callable file-based kernel. Some filenames and tool prefixes retain historical names to preserve links; current headings describe current subjects.

Read docs/AUTHORING.md before changing lessons. Tools live in tools/pNN_name/; CMake discovers them automatically. Do not change shared helper behavior casually: recorded outputs depend on it.

```bash
export LLVM="$(brew --prefix llvm)"
scripts/build.sh
build/bin/p01_engine
build/bin/p07_kernel manifests/p07_kernel.cpp 0 6 -3
scripts/test_kernel.sh
scripts/check.sh --quick
python3 scripts/build_site.py
python3 scripts/check_site.py
```

Markdown is the source of truth. Edit scripts/site_assets/ for site behavior and styling, then regenerate site/; do not edit generated HTML directly. Keep dark mode, collapsible navigation and continuous lesson prose. Native C++ executes in local tools; the browser is a reading, hint and progress interface.

Keep learner changes in copies under out/ or new tool directories. Restore reference examples before checking recorded runs. Never call a pointer after removing its module or engine. Destroy object Values before the interpreter. LLVM 22.1.8's broken Value move constructor requires the demonstrated copy or move-assignment paths.

Open with the concepts and API overview before any build exercise. Keep part and section numbering consecutive; a cleared sidebar filter must restore all lessons.

Publication is separate from a local rebuild; deploy only when requested.
