#!/usr/bin/env python3
"""Compile Markdown-extracted programs and execute their printed build commands."""
import os
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from complete_examples import EXAMPLES, MARKER, ROOT

RESULTS = {
    "1.10": r"(?s)run\(6\) = 42.*run\(5\) = 45.*define.*@run.*compiled; execution skipped",
    "2.2": r"Function add", "2.3": r"__stmts__", "2.4": r"\(int\) 82",
    "3.1": r"100000 calls, final value 5000150000", "3.6": r"\(int\) 2",
    "4.4": r"Inner",
    "1.5": r"captured = 35", "1.6": r"run\(6\) = 42",
    "1.7": r"captured = 35", "1.8": r"after error = 42",
    "2.1": r"quad\(5\) = 20", "3.2": r"after the interpreter added 5",
    "3.3": r"host now sees: verbosity=7", "3.4": r"library result = 42",
    "3.5": r"secret: 42", "4.1": r"inputs=9 failed=4", "4.2": r"demo_total",
    "4.3": r"counter", "4.5": r"8999991", "5.1": r"K_Int",
    "5.2": r"a conversion", "5.3": r"~T", "5.4": r"pretty:",
    "5.5": r"hello", "6.1": r"undo: 6 inputs undone", "6.2": r"Person\{name=Ada",
    "6.3": r"symbols the main JITDylib", "6.4": r"(?:ELF|Mach-O) object",
    "6.5": r"static destructor ran", "6.6": r"JIT pid == the executor we started: yes",
    "6.7": r"pow4\(3\) = 81", "6.8": r"has IR: no",
    "7.1": r"run\(6\) = 42", "7.2": r"run\(6\) = 42",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--section", action="append", choices=sorted(EXAMPLES))
    options = parser.parse_args()
    subprocess.run([sys.executable, ROOT / "scripts/complete_examples.py"], check=True)
    base = ROOT / "out/complete-examples"
    base.mkdir(parents=True, exist_ok=True)
    # The extraction utility requires a fresh destination.
    work = Path(tempfile.mkdtemp(prefix="run-", dir=base)) / "lab"
    subprocess.run([sys.executable, ROOT / "scripts/complete_examples.py", "--extract", work], check=True)
    system = "macOS" if sys.platform == "darwin" else "Linux"
    env = dict(os.environ)
    print(f"Testing standalone sources on {system}: {work}", flush=True)
    failures = []
    tested = 0
    for doc in sorted((ROOT / "docs").glob("part_*.md")):
        for example in MARKER.finditer(doc.read_text()):
            section = example[1]
            if options.section and section not in options.section:
                continue
            tested += 1
            # Execute the actual platform block printed next to the source.
            command = re.search(rf"{system}:\n\n```bash\n(.*?)\n```", example[0], re.S)[1]
            path = work.parent / f"lesson-{section}.log"
            try:
                result = subprocess.run(["bash", "-euo", "pipefail", "-c", command],
                                        cwd=work, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, timeout=180)
                path.write_text(result.stdout)
                ok = result.returncode == 0 and re.search(RESULTS[section], result.stdout)
                for target in EXAMPLES[section][1].split():
                    if target == "p01_engine":
                        continue
                    help_result = subprocess.run([str(work / "out" / target), "--help"],
                                                 cwd=work, env=env, text=True,
                                                 stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT, timeout=30)
                    invalid_result = subprocess.run(
                        [str(work / "out" / target), "--unknown-host-option-for-testing"],
                        cwd=work, env=env, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=30)
                    (work.parent / f"cli-{target}.log").write_text(
                        help_result.stdout + "\nInvalid option:\n" + invalid_result.stdout)
                    ok = (ok and help_result.returncode == 0
                          and "USAGE:" in help_result.stdout
                          and invalid_result.returncode != 0
                          and "Unknown command line argument" in invalid_result.stdout)
                if section == "1.10":
                    # Exercise the generated interface before compiler creation.
                    for arguments, status, message in (
                        (["--help"], 0, "compile"),
                        (["run", "--help"], 0, "--input"),
                        (["compile", "--help"], 0, "--dump-ir"),
                        ([], 1, "Choose a command"),
                        (["run", "--input=banana"], 1, "invalid"),
                        (["run", "--inpoot=6"], 1, "Unknown command line argument"),
                        (["compile", "--input=6"], 1, "Unknown command line argument"),
                        (["run", "--", "-std=not-a-cxx-standard"], 1, "invalid value"),
                    ):
                        parsed = subprocess.run([str(work / "out/p01_options"), *arguments],
                                                cwd=work, env=env, text=True,
                                                stdout=subprocess.PIPE,
                                                stderr=subprocess.STDOUT, timeout=30)
                        with path.open("a") as log:
                            log.write(f"\nparser case: {arguments!r}\n{parsed.stdout}")
                        ok = (ok and parsed.returncode == status and message in parsed.stdout
                              and "cl::alias must not" not in parsed.stdout)
                # These lessons intentionally demonstrate compiler diagnostics.
                if section not in {"1.8", "2.1", "3.6", "4.1", "6.1", "6.6", "6.8"}:
                    ok = ok and not re.search(r"(^|\s)error:|LLVM ERROR:", result.stdout)
            except subprocess.TimeoutExpired:
                ok = False
                path.write_text("Timed out: the example did not finish within 180 seconds.\n")
            print(f"{'PASS' if ok else 'FAIL'} {system} lesson {section}", flush=True)
            if not ok:
                failures.append(f"{section} ({path})")
    if failures:
        raise SystemExit("Failed examples: " + ", ".join(failures))
    print(f"{system}: all {tested} selected lesson command blocks passed; logs: {work.parent}", flush=True)


if __name__ == "__main__":
    main()
