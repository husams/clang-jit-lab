#!/usr/bin/env python3
"""Validate the documentation set.

  * every relative markdown link in README.md, CLAUDE.md and docs/*.md resolves
  * every `## Section N.M` heading in docs/part_N_*.md has a line
    `- [ ] N.M — ...` (or `[x]`) in docs/PROGRESS.md with the same title, and
    vice versa
  * every part file is listed in docs/README.md
  * nav links: each part has prev/next links at the top and bottom

Usage: scripts/check_links.py          (exit 1 on any problem)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
problems = []


def err(msg):
    problems.append(msg)


def md_files():
    yield ROOT / "README.md"
    yield ROOT / "CLAUDE.md"
    yield from sorted(DOCS.glob("*.md"))


link_re = re.compile(r"\]\(([^)\s]+)\)")
for f in md_files():
    if not f.exists():
        err(f"missing file: {f.relative_to(ROOT)}")
        continue
    text = f.read_text()
    # ignore fenced code
    text_nocode = re.sub(r"```.*?```", "", text, flags=re.S)
    for m in link_re.finditer(text_nocode):
        tgt = m.group(1)
        if re.match(r"^(https?:|mailto:|#)", tgt):
            continue
        path = (f.parent / tgt.split("#")[0]).resolve()
        if not path.exists():
            err(f"{f.relative_to(ROOT)}: broken link -> {tgt}")

# part files and PROGRESS
parts = sorted(DOCS.glob("part_[0-9]*_*.md"), key=lambda p: int(re.match(r"part_(\d+)_", p.name).group(1)))
progress = (DOCS / "PROGRESS.md").read_text() if (DOCS / "PROGRESS.md").exists() else ""
prog_items = dict(re.findall(r"^- \[[ x]\] (\d+\.\d+) — (.+?)\s*$", progress, flags=re.M))
readme = (DOCS / "README.md").read_text() if (DOCS / "README.md").exists() else ""

seen = set()
part_numbers = [int(re.match(r"part_(\d+)_", p.name).group(1)) for p in parts]
if part_numbers != list(range(1, len(parts) + 1)):
    err(f"parts must appear consecutively from 1, found {part_numbers}")
for p in parts:
    n = re.match(r"part_(\d+)_", p.name).group(1)
    text = p.read_text()
    h1 = re.search(r"^# (.+?)\s*$", text, flags=re.M)
    if not h1 or not h1.group(1).startswith(f"Part {n} — "):
        err(f"{p.name}: first heading must be '# Part {n} — Title'")
    else:
        if f"## {h1.group(1)}" not in progress:
            err(f"PROGRESS.md has no '## {h1.group(1)}' heading")
        if f"**{h1.group(1)}**" not in readme:
            err(f"docs/README.md has no '**{h1.group(1)}**' section-list heading")
    if p.name not in readme:
        err(f"docs/README.md does not link {p.name}")
    heads = re.findall(r"^## Section (\d+\.\d+) — (.+?)\s*$", text, flags=re.M)
    ordered = [num for num, _ in heads]
    if ordered != [f"{n}.{i}" for i in range(1, len(heads) + 1)]:
        err(f"{p.name}: missing, duplicate or out-of-order section: {ordered}")
    if not heads:
        err(f"{p.name}: no '## Section N.M — Title' headings")
    for num, title in heads:
        seen.add(num)
        if not num.startswith(n + "."):
            err(f"{p.name}: section {num} in part {n}")
        if num not in prog_items:
            err(f"PROGRESS.md missing {num}")
        elif prog_items[num].strip() != title.strip():
            err(f"PROGRESS.md title for {num} differs: {prog_items[num]!r} vs {title!r}")
    # nav links top and bottom
    nav = re.findall(r"^\[← .*\]\(.*\) \| \[.* →\]\(.*\)$|^\[← .*\]\(.*\) \| \[README\]\(README.md\)$", text, flags=re.M)
    if len(nav) < 2:
        err(f"{p.name}: expected prev/next navigation at top and bottom, found {len(nav)}")
for num in prog_items:
    if num not in seen:
        err(f"PROGRESS.md lists {num} but no part file has that section")

# docs/README.md section lists must agree with PROGRESS.md
readme_items = dict(re.findall(r"^- (\d+\.\d+) (.+?)\s*$", readme, flags=re.M))
for num, title in prog_items.items():
    if num not in readme_items:
        err(f"docs/README.md section list is missing {num}")
    elif readme_items[num].strip() != title.strip():
        err(f"docs/README.md title for {num} differs: {readme_items[num]!r} vs {title!r}")
for num in readme_items:
    if num not in prog_items:
        err(f"docs/README.md lists {num} which PROGRESS.md does not have")

# Reorganized lessons must not leave numbered references to removed sections.
for p in parts:
    prose = re.sub(r"```.*?```", "", p.read_text(), flags=re.S)
    for num in re.findall(r"\bSection (\d+\.\d+)\b", prose):
        if num not in seen:
            err(f"{p.name}: reference to missing Section {num}")
    for title in re.findall(r"^#{2,6}\s+(.+?)\s*$", prose, flags=re.M):
        if title.lower() in {"why", "what to do", "verify", "expected"}:
            err(f"{p.name}: generic lesson heading {title!r}")

print(f"check_links: {len(list(md_files()))} markdown files, {len(parts)} part files, {len(problems)} problem(s)")
for p in problems:
    print("  " + p)
sys.exit(1 if problems else 0)
