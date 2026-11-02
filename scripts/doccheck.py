#!/usr/bin/env python3
"""Run the commands in the lab docs and check (or refresh) their Expected output.

Convention (see docs/AUTHORING.md):

    ```bash
    build/bin/p02_walk manifests/p01_hello.cpp --func=sign
    ```

    ```text expected
    == sign: 5 blocks
    ...
    ```

A fenced block whose info string is `text expected` is the recorded output of
the fenced `bash` block immediately before it. Every `bash` block that has no
`text expected` block after it is left alone (build steps, long runs, ...).
Bare ``` blocks and ```dot diagrams are skipped entirely: they never count as
the block "directly after" a command.

Each bash block runs in a fresh `bash -c` with the lab root as the working
directory, stdout and stderr merged. Absolute lab paths are rewritten to
relative ones before comparing, so docs never contain /Users/<name>/...

Usage:
    scripts/doccheck.py docs/part_1_*.md          # check; exit 1 on any mismatch
    scripts/doccheck.py --fill docs/part_2_*.md   # rewrite Expected blocks in place
    scripts/doccheck.py --list docs/part_2_*.md   # list the commands that would run
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FENCE = re.compile(r"^```(.*)$")


def parse(text):
    """Return a list of blocks: dict(info, start, end, lines) with line indices
    of the opening fence (start) and closing fence (end)."""
    lines = text.split("\n")
    blocks, i = [], 0
    while i < len(lines):
        m = FENCE.match(lines[i])
        info = m.group(1).strip() if m else ""
        if m and info and info.split()[0] != "dot":
            start = i
            i += 1
            body = []
            while i < len(lines) and lines[i].rstrip() != "```":
                body.append(lines[i])
                i += 1
            blocks.append({"info": info, "start": start, "end": i, "lines": body})
        elif m:  # a bare ``` opener or a ```dot diagram: skip its body
            i += 1
            while i < len(lines) and lines[i].rstrip() != "```":
                i += 1
        i += 1
    return lines, blocks


def run(cmd):
    (ROOT / "out").mkdir(exist_ok=True)  # scratch dir used by doc commands
    env = dict(os.environ)
    env.setdefault("LLVM", "/opt/homebrew/opt/llvm")
    p = subprocess.run(
        ["bash", "-c", cmd], cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=300,
    )
    out = p.stdout.replace(str(ROOT) + "/", "").replace(str(ROOT), ".")
    return [l.rstrip() for l in out.rstrip("\n").split("\n")] if out.strip() else []


def pairs(blocks):
    for prev, cur in zip(blocks, blocks[1:]):
        if cur["info"].split()[:2] == ["text", "expected"] and prev["info"].split()[0] == "bash":
            yield prev, cur


def main(argv):
    fill = "--fill" in argv
    listing = "--list" in argv
    files = [a for a in argv if not a.startswith("--")]
    bad = 0
    total = 0
    for f in files:
        path = Path(f)
        text = path.read_text()
        lines, blocks = parse(text)
        # orphan expected blocks are an authoring error
        paired = {id(c) for _, c in pairs(blocks)}
        for b in blocks:
            if b["info"].split()[:2] == ["text", "expected"] and id(b) not in paired:
                print(f"{f}:{b['start']+1}: `text expected` block not directly after a bash block")
                bad += 1
        edits = []
        for cmdb, expb in pairs(blocks):
            total += 1
            cmd = "\n".join(cmdb["lines"])
            if listing:
                print(f"{f}:{cmdb['start']+1}: {cmd.splitlines()[0]}")
                continue
            actual = run(cmd)
            want = [l.rstrip() for l in expb["lines"]]
            while want and not want[-1]:
                want.pop()
            if actual == want:
                continue
            if fill:
                edits.append((expb["start"] + 1, expb["end"], actual))
            else:
                bad += 1
                print(f"MISMATCH {f}:{cmdb['start']+1}: {cmd.splitlines()[0]}")
                import difflib
                for d in list(difflib.unified_diff(want, actual, "docs", "actual", lineterm="", n=1))[:30]:
                    print("   " + d)
        if fill and edits:
            for s, e, new in sorted(edits, reverse=True):
                lines[s:e] = new
            path.write_text("\n".join(lines))
            print(f"{f}: refreshed {len(edits)} expected block(s)")
    if not fill and not listing:
        print(f"doccheck: {total} command(s) checked, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
