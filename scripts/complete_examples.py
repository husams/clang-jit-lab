#!/usr/bin/env python3
"""Keep complete lesson programs and their platform commands in sync.

Use --write after changing a listed source; the default checks both editions'
Markdown source. --extract copies a buildable source tree, then overwrites its
example files with the actual Markdown fences for independent compilation.
"""
import argparse
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# section: (complete source files, build targets, execution commands)
EXAMPLES = {
    "1.10": (["examples/p01_options.cpp"], "p01_options", "build/bin/p01_options run --input=6\nbuild/bin/p01_options run -x=5 -- -DSCALE=9\nbuild/bin/p01_options compile --dump-ir"),
    "1.5": (["examples/p01_engine.cpp"], "p01_engine", "build/bin/p01_engine"),
    "1.6": (["examples/p01_call.cpp"], "p01_call", "build/bin/p01_call"),
    "1.7": (["examples/p01_capture.cpp"], "p01_capture", "build/bin/p01_capture"),
    "1.8": (["examples/p01_errors.cpp"], "p01_errors", "build/bin/p01_errors"),
    "2.1": (["examples/p04_parse.cpp"], "p04_parse", "build/bin/p04_parse"),
    "3.2": (["examples/p05_shared.cpp"], "p05_shared", "build/bin/p05_shared --mode=extern"),
    "3.3": (["examples/p05_inject.cpp"], "p05_inject", "build/bin/p05_inject --mode=config"),
    "3.4": (["examples/support_library.cpp", "examples/p03_library.cpp"], "p03_library", "LIBRARY"),
    "3.5": (["examples/p06_jit.cpp"], "p06_jit", "build/bin/p06_jit --abs"),
    "4.1": (["examples/p04_diag.cpp", "examples/p04_errors.cpp"], "p04_diag p04_errors", "build/bin/p04_diag -- -Wall\nbuild/bin/p04_errors"),
    "4.2": (["examples/p04_complete.cpp"], "p04_complete", "build/bin/p04_complete --prefix=demo_ --prefix=demo_point."),
    "4.3": (["examples/p05_lookup.cpp"], "p05_lookup", "build/bin/p05_lookup --mode=lookup --name=counter --name=ratio --name=over --name=Point"),
    "4.5": (["examples/p05_templates.cpp"], "p05_templates", "build/bin/p05_templates --mode=address --type=int --type=double"),
    "5.1": (["examples/p05_anatomy.cpp"], "p05_anatomy", "build/bin/p05_anatomy"),
    "5.2": (["examples/p05_kinds.cpp"], "p05_kinds", "build/bin/p05_kinds --mode=table\nbuild/bin/p05_kinds --mode=natural\nbuild/bin/p05_kinds --mode=wrong"),
    "5.3": (["examples/p05_ownership.cpp"], "p05_ownership", "build/bin/p05_ownership --mode=safe"),
    "5.4": (["examples/p05_printing.cpp"], "p05_printing", "build/bin/p05_printing --mode=pretty"),
    "5.5": (["examples/p05_inspect.cpp"], "p05_inspect", "build/bin/p05_inspect --expression=scene --expression=small --expression=big"),
    "6.1": (["examples/p06_consumer.cpp"], "p06_consumer", "build/bin/p06_consumer"),
    "6.2": (["examples/p06_inject.cpp"], "p06_inject", "build/bin/p06_inject"),
    "6.3": (["examples/p06_jit.cpp"], "p06_jit", "build/bin/p06_jit --abs --trace"),
    "6.4": (["examples/p06_objects.cpp"], "p06_objects", "build/bin/p06_objects --output-dir=out/objects"),
    "6.5": (["examples/p06_executor.cpp"], "p06_executor", "build/bin/p06_executor"),
    "6.6": (["examples/p06_oop.cpp"], "p06_oop", "build/bin/p06_oop --mode=spawn\nbuild/bin/p06_oop --mode=connect"),
    "6.7": (["examples/p06_eval.cpp"], "p06_eval", "build/bin/p06_eval"),
    "6.8": (["examples/p06_check.cpp"], "p06_check", "build/bin/p06_check"),
    "7.1": (["examples/kernel.cpp"], "p07_kernel", "build/bin/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3"),
    "7.2": (["examples/p07_kernel.cpp"], "p07_kernel", "build/bin/p07_kernel --source=examples/kernel.cpp --input=0 --input=6 --input=-3"),
    "2.2": (["examples/p04_ast.cpp"], "p04_ast", "build/bin/p04_ast --dump=4 --dump=5"),
    "2.3": (["examples/p04_ir.cpp"], "p04_ir", "build/bin/p04_ir"),
    "2.4": (["examples/p04_hello.cpp"], "p04_hello", "build/bin/p04_hello"),
    "3.1": (["examples/p04_call.cpp"], "p04_call", "build/bin/p04_call"),
    "3.6": (["examples/p04_undo.cpp"], "p04_undo", "build/bin/p04_undo"),
    "4.4": (["examples/p05_layout.cpp"], "p05_layout", "build/bin/p05_layout --mode=layout --name=Inner --name=Layout"),
}
MARKER = re.compile(r"<!-- runnable-example: ([\d.]+) -->\n.*?<!-- /runnable-example -->", re.S)
CPP = re.compile(r"^```(?:cpp|c\+\+|cxx|cc|c)(?:[ \t][^\n]*)?\n(.*?)^```[ \t]*$", re.M | re.S)


def render(section):
    sources, targets, run = EXAMPLES[section]
    blocks = [f"<!-- runnable-example: {section} -->"]
    for source in sources:
        blocks.append(f"`{source}`:\n\n```cpp\n{(ROOT / source).read_text().rstrip()}\n```")
    for system, prefix in (("Linux", "llvm-config-22 --prefix"), ("macOS", "brew --prefix llvm")):
        compile_flags = "-Wl,--export-dynamic " if system == "Linux" else ""
        builds = []
        for target in targets.split():
            builds.append(f'"$LLVM/bin/clang++" $("$LLVM/bin/llvm-config" --cxxflags) -std=c++20 -pthread \\\n'
                          f'  examples/{target}.cpp -L"$LLVM/lib" -Wl,-rpath,"$LLVM/lib" \\\n'
                          f'  {compile_flags}-lclang-cpp -lLLVM -o out/{target}')
        build = "\n".join(builds)
        flags = '-std=c++20 -resource-dir "$("$LLVM/bin/clang++" -print-resource-dir)"'
        if system == "Linux":
            flags += ' --gcc-toolchain=/usr'
        else:
            flags += ' -nostdinc++ -isystem "$LLVM/include/c++/v1" -isysroot "$(xcrun --show-sdk-path)"'
        command = run.replace("build/bin/", "out/")
        if run == "LIBRARY":
            library_flags, ext = ("-fPIC -shared", "so") if system == "Linux" else ("-dynamiclib", "dylib")
            command = f'"$LLVM/bin/clang++" {library_flags} examples/support_library.cpp -o out/p03_library.{ext}\nout/p03_library --library=out/p03_library.{ext}'
        lines = []
        for line in command.splitlines():
            if line.startswith("out/"):
                invocation, _, extra_flags = line.partition(" -- ")
                line = invocation + " -- " + flags + (" " + extra_flags if extra_flags else "")
            lines.append(line)
        command = "\n".join(lines)
        blocks.append(f'{system}:\n\n```bash\nexport LLVM="${{LLVM:-$({prefix})}}"\nmkdir -p out\n{build}\n{command}\n```')
    blocks.append("<!-- /runnable-example -->")
    return "\n\n".join(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--extract", type=Path)
    args = parser.parse_args()
    seen, extracted = set(), {}
    for doc in sorted((ROOT / "docs").glob("part_*.md")):
        text = doc.read_text()
        for match in MARKER.finditer(text):
            section = match[1]
            if section in seen or section not in EXAMPLES:
                raise SystemExit(f"Invalid or duplicate example: {doc.name} {section}")
            seen.add(section)
            expected = render(section)
            if not args.write and match[0] != expected:
                raise SystemExit(f"{doc.name} {section}: run python3 scripts/complete_examples.py --write")
            codes = CPP.findall(expected if args.write else match[0])
            if len(codes) != len(EXAMPLES[section][0]):
                raise SystemExit(f"{doc.name} {section}: missing complete source fence")
            for source, code in zip(EXAMPLES[section][0], codes):
                if "int main(" in code and source != "examples/p01_engine.cpp":
                    if "llvm::cl::ParseCommandLineOptions" not in code or "<llvm/Support/CommandLine.h>" not in code:
                        raise SystemExit(f"{source}: use LLVM's command-line parser")
                    if re.search(r'\bargv\s*\[|\bAfterSeparator\b|\b(?:stoi|stoul|from_chars)\s*\(', code):
                        raise SystemExit(f"{source}: manual command-line parsing is forbidden")
                if re.search(r'#[ \t]*include[ \t]*[<"](?:repllab|kinds|socket_transport)\.h|REPLLAB_|repllab::', code):
                    raise SystemExit(f"{source}: project-specific setup is forbidden in lesson code")
                if re.search(r'\b(?:makeCompiler|makeInterpreter|splitArgs|sampleInputs|valueString|kindName|Check|ExitOnErr)\s*\(|\bExitOnError\b|\bstatic\s+bool\s+(?:run|eval)\s*\(|\bauto\s+eval\s*=', code):
                    raise SystemExit(f"{source}: API and error-handling wrappers are forbidden in lesson code")
                if code != (ROOT / source).read_text().rstrip() + "\n":
                    raise SystemExit(f"{doc.name} {section}: extracted source differs from {source}")
                if source in extracted and extracted[source] != code:
                    raise SystemExit(f"Different copies of {source}")
                extracted[source] = code
        if CPP.search(MARKER.sub("", text)):
            raise SystemExit(f"{doc.name}: C++ example outside the complete-program registry")
        if args.write:
            doc.write_text(MARKER.sub(lambda m: render(m[1]), text))
    if seen != set(EXAMPLES):
        raise SystemExit(f"Missing examples: {sorted(set(EXAMPLES) - seen)}")
    if args.extract:
        dest = args.extract.resolve()
        if dest == ROOT or ROOT not in dest.parents or dest.exists():
            raise SystemExit("Extraction needs a new directory beneath the lab root")
        for folder in ("examples",):
            shutil.copytree(ROOT / folder, dest / folder,
                            ignore=shutil.ignore_patterns("build", "__pycache__", ".DS_Store"))
        for source, code in extracted.items():
            (dest / source).write_text(code)
        print(f"Extracted {len(extracted)} complete files to {dest}")
    print(f"Complete examples: {len(seen)} lessons, {len(extracted)} distinct source files, both platform commands checked")


if __name__ == "__main__":
    main()
