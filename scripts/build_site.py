#!/usr/bin/env python3
"""Generate the interactive HTML edition of the lab from docs/*.md.

    python3 scripts/build_site.py        # writes site/index.html + site/part_N.html
    open site/index.html

The markdown is the single source of truth; the site is rebuilt from it every
time (scripts/check.sh does that and then verifies it with check_site.py), so
the two cannot drift. Pure Python 3 standard library: a small purpose-built
markdown converter handles exactly the dialect the docs use (fenced code with
info strings, tables, headings, lists, Obsidian callouts, links, inline code).

Features of the output: dark theme, sidebar that collapses to a
slim part rail ("[" toggles) with expandable section lists and a filter box,
mobile hamburger, scroll-spy,
prev/next part navigation, copy buttons, locally bundled highlight.js,
per-section "done" checkboxes + progress bar (localStorage),
continuous lesson prose, folded long recorded outputs,
quizzes and answers as folded callouts; ```dot fences become inline,
interactive SVG diagrams (pan/zoom, hover/pin highlight, full screen, source,
download) rendered at build time with Graphviz `dot` (brew install graphviz).
"""
import html
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import outviz  # graph views of `text expected` blocks (scripts/outviz/)

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SITE = ROOT / "site"
ASSETS = Path(__file__).resolve().parent / "site_assets"
ASSET_VERSION = hashlib.sha256(
    (ASSETS / "style.css").read_bytes() + (ASSETS / "app.js").read_bytes()
    + (ASSETS / "vendor" / "highlight.min.js").read_bytes()
).hexdigest()[:12]

LONG_EXPECTED_LINES = 30  # `text expected` blocks longer than this fold by default
LONG_CODE_LINES = 45  # code blocks longer than this get a max-height + expand button

# Defaults for every ```dot diagram, so the sources carry no fonts/colors/sizes.
# Colors come from the site CSS, keyed on the `class=` attributes Graphviz copies onto the SVG.
DOT_FLAGS = [
    "-Gbgcolor=transparent", "-Gpad=0.15", "-Gnodesep=0.35", "-Granksep=0.45", "-Gfontname=Helvetica",
    "-Nfontname=Menlo", "-Efontname=Helvetica", "-Gfontsize=12", "-Nfontsize=12", "-Efontsize=11",
    "-Nshape=box", "-Nstyle=rounded", "-Nmargin=0.12,0.06", "-Earrowsize=0.7",
]
GV_NATURAL_SCALE = 0.85  # a diagram is never shown larger than this fraction of Graphviz's own 96-dpi size

# toolbar of a diagram: (data-act, aria-label, icon path on a 16x16 grid)
GV_BUTTONS = [
    ("zoom-out", "Zoom out", "M3 8h10"),
    ("zoom-in", "Zoom in", "M8 3v10M3 8h10"),
    ("fit", "Fit to view", "M2 6V2h4M10 2h4v4M14 10v4h-4M6 14H2v-4"),
    ("expand", "Expand to full screen", "M9.5 2.5h4v4M13.5 2.5l-5 5M6.5 13.5h-4v-4M2.5 13.5l5-5"),
    ("download", "Download SVG", "M8 2.5v8M4.5 7.5L8 11l3.5-3.5M3 13.5h10"),
]
GV_CLOSE_ICON = "M4 4l8 8M12 4l-8 8"

warnings: list[str] = []


class DotError(Exception):
    """A ```dot block could not be rendered; the build stops (no silent fallback)."""


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def esc(s: str) -> str:
    return html.escape(s, quote=True)


def slug(text: str) -> str:
    t = text.replace("`", "").lower()
    t = re.sub(r"[^a-z0-9]+", "-", t).strip("-")
    return t or "x"


def plain(text: str) -> str:
    """Heading/inline markdown -> plain text (for ids, search, <title>)."""
    t = re.sub(r"`+([^`]*)`+", r"\1", text)
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", t)
    return t.replace("**", "").replace("*", "")


def part_files() -> list[Path]:
    fs = [p for p in DOCS.glob("part_[0-9]*_*.md")]
    return sorted(fs, key=lambda p: int(re.match(r"part_(\d+)_", p.name).group(1)))


def part_no(p: Path) -> int:
    return int(re.match(r"part_(\d+)_", p.name).group(1))


# --------------------------------------------------------------------------
# inline markdown
# --------------------------------------------------------------------------
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)", re.S)


class Ctx:
    """Per-page conversion context."""

    def __init__(self, md_name: str, home: bool, parts: dict[str, int]):
        self.md_name = md_name
        self.home = home
        self.parts = parts  # md filename -> part number
        self.used_ids: set[str] = set()
        self.part_n = parts.get(md_name)
        self.gv = 0  # diagrams rendered on this page (id prefix gvN-)
        self.cmds: dict[int, str] = {}  # `text expected` fence line -> the bash command it records (outviz.expected_commands)
        self.graph_view = False  # set by render_code when it produced a graph view; assemble() reads and clears it

    def uid(self, base: str) -> str:
        i, cand = 2, base
        while cand in self.used_ids:
            cand = f"{base}-{i}"
            i += 1
        self.used_ids.add(cand)
        return cand


def rewrite_href(url: str, ctx: Ctx):
    """Return (href, kind): kind is 'int', 'ext' or 'none' (unlinkable)."""
    if re.match(r"^(https?:|mailto:)", url):
        return url, "ext"
    if url.startswith("#"):
        return url, "int"
    path, _, frag = url.partition("#")
    frag = ("#" + frag) if frag else ""
    name = Path(path).name
    if path in (name, "./" + name) and name in ctx.parts:
        return f"part_{ctx.parts[name]}.html{frag}", "int"
    if path in (name, "./" + name) and name == "README.md":
        return f"index.html{frag}", "int"
    if not url.startswith("../"):  # links into sibling labs are intentionally not part of the site
        warnings.append(f"{ctx.md_name}: link to {url!r} is outside the site; rendered as plain text")
    return url, "none"


def inline(s: str, ctx: Ctx) -> str:
    codes: list[str] = []

    def take(m):
        body = m.group(2)
        if body.startswith(" ") and body.endswith(" ") and body.strip():
            body = body[1:-1]
        nw = ' class="nw"' if len(body) <= 32 else ""
        codes.append(f"<code{nw}>{esc(body)}</code>")
        return f"\x00{len(codes) - 1}\x00"

    s = CODE_SPAN.sub(take, s)
    s = html.escape(s, quote=False)

    def link(m):
        text, url = m.group(1), m.group(2)
        href, kind = rewrite_href(html.unescape(url), ctx)
        if kind == "none":
            return f'<span class="nolink" title="{esc(href)}">{text}</span>'
        ext = ' target="_blank" rel="noopener"' if kind == "ext" else ""
        return f'<a href="{esc(href)}"{ext}>{text}</a>'

    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)
    s = re.sub(r"\*\*(?=\S)(.+?)(?<=\S)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"(?<![\w*])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![\w*])", r"<em>\1</em>", s)
    s = re.sub(r"(?<![\w])_(?=[^\s_])(.+?)(?<=[^\s_])_(?![\w])", r"<em>\1</em>", s)
    return re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], s)


# --------------------------------------------------------------------------
# block markdown
# --------------------------------------------------------------------------
FENCE = re.compile(r"^(\s*)(`{3,}|~{3,})\s*(.*?)\s*$")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
HR = re.compile(r"^\s*(?:-{3,}|\*{3,}|_{3,})\s*$")
LIST_ITEM = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$")
TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$")
NAV_PARA = re.compile(r"^\[← [^\]]+\]\([^)]+\) \| \[[^\]]+\]\([^)]+\)$")
CALLOUT = re.compile(r"^\[!(\w+)\]([+-]?)\s*(.*)$")


def split_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    cells, cur, i, in_code = [], "", 0, 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            cur += "|"
            i += 2
            continue
        if c == "`":
            j = i
            while j < len(line) and line[j] == "`":
                j += 1
            run = j - i
            in_code = 0 if in_code == run else (run if not in_code else in_code)
            cur += line[i:j]
            i = j
            continue
        if c == "|" and not in_code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    cells.append(cur.strip())
    return cells


def starts_block(line: str, nxt: str | None) -> bool:
    if FENCE.match(line) or HEADING.match(line) or HR.match(line) or line.lstrip().startswith(">"):
        return True
    m = LIST_ITEM.match(line)
    if m and (m.group(2) in "-*+" or m.group(2).startswith("1")):
        return True
    if "|" in line and nxt is not None and "|" in nxt and TABLE_SEP.match(nxt):
        return True
    return False


def parse(lines: list[str], ctx: Ctx, top: bool = False, base: int = 0) -> list:
    """Markdown lines -> list of items: html strings or tuples
    ('h', level, raw), ('hr',), ('code', info, text, md_line). `base` is the
    0-based file line of lines[0] (nested quote/list bodies keep their line numbers)."""
    out: list = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        # Source-synchronization markers belong to the authoring format only.
        if re.fullmatch(r"<!-- (?:runnable-example: [\d.]+|/runnable-example) -->", line.strip()):
            i += 1
            continue
        fm = FENCE.match(line)
        if fm:
            indent, fence, info = len(fm.group(1)), fm.group(2), fm.group(3)
            body: list[str] = []
            start = i
            i += 1
            closer = re.compile(r"^\s*" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}\s*$")
            while i < n and not closer.match(lines[i]):
                body.append(lines[i][indent:] if lines[i][:indent].strip() == "" else lines[i])
                i += 1
            i += 1
            out.append(("code", info, "\n".join(body), base + start + 1))
            continue
        hm = HEADING.match(line)
        if hm:
            out.append(("h", len(hm.group(1)), hm.group(2)))
            i += 1
            continue
        if HR.match(line):
            out.append(("hr",))
            i += 1
            continue
        if "|" in line and i + 1 < n and "|" in lines[i + 1] and TABLE_SEP.match(lines[i + 1]):
            head = split_row(line)
            aligns = []
            for c in split_row(lines[i + 1]):
                aligns.append("center" if c.startswith(":") and c.endswith(":") else "right" if c.endswith(":") else "")
            i += 2
            rows = []
            while i < n and lines[i].strip() and "|" in lines[i]:
                rows.append(split_row(lines[i]))
                i += 1

            def cell(tag, text, k):
                st = f' style="text-align:{aligns[k]}"' if k < len(aligns) and aligns[k] else ""
                return f"<{tag}{st}>{inline(text, ctx)}</{tag}>"

            t = ['<div class="table-wrap"><table><thead><tr>']
            t += [cell("th", c, k) for k, c in enumerate(head)]
            t.append("</tr></thead><tbody>")
            for r in rows:
                t.append("<tr>" + "".join(cell("td", c, k) for k, c in enumerate(r)) + "</tr>")
            t.append("</tbody></table></div>")
            out.append("".join(t))
            continue
        if line.lstrip().startswith(">"):
            q, q0 = [], i
            while i < n and lines[i].lstrip().startswith(">"):
                q.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append(render_quote(q, ctx, base + q0))
            continue
        lm = LIST_ITEM.match(line)
        if lm:
            i, h = parse_list(lines, i, ctx, base)
            out.append(h)
            continue
        # paragraph
        para = [line.strip()]
        i += 1
        while i < n and lines[i].strip() and not starts_block(lines[i], lines[i + 1] if i + 1 < n else None):
            para.append(lines[i].strip())
            i += 1
        text = " ".join(para)
        if top and NAV_PARA.match(text):
            continue  # replaced by the generated pager
        out.append(f"<p>{inline(text, ctx)}</p>")
    return out


def render_quote(q: list[str], ctx: Ctx, base: int = 0) -> str:
    m = CALLOUT.match(q[0]) if q else None
    if not m:
        return "<blockquote>" + render_flat(parse(q, ctx, base=base), ctx) + "</blockquote>"
    kind, fold, title = m.group(1).lower(), m.group(2), m.group(3).strip()
    body = render_flat(parse(q[1:], ctx, base=base + 1), ctx)
    title_html = inline(title, ctx) if title else esc(kind.capitalize())
    cls = f"callout c-{esc(kind)}"
    if fold:
        op = " open" if fold == "+" else ""
        return (f'<details class="{cls}"{op}><summary><span class="ctype">{esc(kind)}</span>'
                f'<span class="ctitle">{title_html}</span></summary><div class="callout-body">{body}</div></details>')
    return (f'<div class="{cls}"><div class="callout-title"><span class="ctype">{esc(kind)}</span>'
            f'<span class="ctitle">{title_html}</span></div><div class="callout-body">{body}</div></div>')


def parse_list(lines: list[str], i: int, ctx: Ctx, line0: int = 0):
    n = len(lines)
    m = LIST_ITEM.match(lines[i])
    base = len(m.group(1))
    ordered = m.group(2)[0].isdigit()
    items: list[list[str]] = []
    starts: list[int] = []  # file line index of each item's first line
    while i < n:
        m = LIST_ITEM.match(lines[i])
        if not m or len(m.group(1)) != base or m.group(2)[0].isdigit() != ordered:
            break
        off = len(m.group(1)) + len(m.group(2)) + 1
        first = lines[i][off:].lstrip() if lines[i][off:].strip() else ""
        item = [first]
        starts.append(line0 + i)
        i += 1
        while i < n:
            ln = lines[i]
            if not ln.strip():
                # blank: continue only if the next non-blank line is indented deeper, or another item
                j = i
                while j < n and not lines[j].strip():
                    j += 1
                if j < n and (len(lines[j]) - len(lines[j].lstrip()) > base):
                    item.append("")
                    i += 1
                    continue
                break
            indent = len(ln) - len(ln.lstrip())
            if indent > base:
                item.append(ln[min(indent, off):] if indent >= off else ln.lstrip())
                i += 1
                continue
            if LIST_ITEM.match(ln):
                break
            if item and item[-1] != "" and not starts_block(ln, None):
                item.append(ln.strip())  # lazy continuation
                i += 1
                continue
            break
        items.append(item)
        # allow one blank line between items of the same list
        j = i
        while j < n and not lines[j].strip():
            j += 1
        if j < n and j > i:
            nm = LIST_ITEM.match(lines[j])
            if nm and len(nm.group(1)) == base and nm.group(2)[0].isdigit() == ordered:
                i = j
    tag = "ol" if ordered else "ul"
    lis = []
    for it, it0 in zip(items, starts):
        sub = parse(it, ctx, base=it0)
        inner = render_flat(sub, ctx)
        if len(sub) == 1 and isinstance(sub[0], str) and sub[0].startswith("<p>") and sub[0].count("<p>") == 1:
            inner = sub[0][3:-4]
        if ctx.home and not ordered:
            hm = re.match(r"^(\d+)\.(\d+) (.*)$", it[0]) if it and it[0] else None
            if hm:
                inner = (f'<a href="part_{hm.group(1)}.html#s{hm.group(1)}-{hm.group(2)}">'
                         f'<span class="secnum">{hm.group(1)}.{hm.group(2)}</span> {inline(hm.group(3), ctx)}</a>')
        lis.append(f"<li>{inner}</li>")
    return i, f"<{tag}>{''.join(lis)}</{tag}>"


def dot_svg(src: str, where: str, line: int, derived: bool = False) -> str:
    """Run Graphviz on one diagram source; any failure stops the build.
    `derived`: the source was generated from a `text expected` block (outviz), so `line` offsets nothing."""
    try:
        p = subprocess.run(["dot", "-Tsvg", *DOT_FLAGS], input=src, capture_output=True, text=True)
    except FileNotFoundError:
        raise DotError(f"{where}: Graphviz `dot` not found on PATH (brew install graphviz)") from None
    err = p.stderr.strip() if derived else re.sub(r"\bline (\d+)", lambda m: f"line {int(m.group(1)) + line} (block line {m.group(1)})", p.stderr.strip())
    if p.returncode != 0:
        gen = f"\ngenerated dot source:\n{src}" if derived else ""
        raise DotError(f"{where}: dot exited {p.returncode}:\n{err}{gen}")
    if p.stdout.count("<svg") != 1:
        raise DotError(f"{where}: a dot block must hold exactly one graph")
    if err:
        warnings.append(f"{where}: dot: {err}")
    return p.stdout


def finish_svg(svg: str, prefix: str, label: str) -> str:
    """Graphviz SVG -> inline-ready SVG: no prolog/comments, scales with its box, ids unique per page."""
    svg = re.sub(r"<\?xml.*?\?>|<!DOCTYPE.*?>|<!--.*?-->", "", svg, flags=re.S).strip()
    m = re.match(r"<svg\b([^>]*)>", svg)
    attrs = re.sub(r"\s+", " ", m.group(1))
    vb = re.search(r'viewBox="[\d.\-]+ [\d.\-]+ [\d.\-]+ ([\d.\-]+)"', attrs)
    nat_h = round(float(vb.group(1)) * 96 / 72 * GV_NATURAL_SCALE) if vb else 600
    attrs = re.sub(r'\s(?:width|height)="[^"]*"', "", attrs)
    svg = (f'<svg{attrs} class="gv-svg" role="group" aria-label="{esc(label)}" tabindex="-1" focusable="false" '
           f'style="--gv-h:{nat_h}px">' + svg[m.end():])
    ids = set(re.findall(r'(?<=\s)id="([^"]*)"', svg))
    svg = re.sub(r'(?<=\s)id="([^"]*)"', lambda k: f'id="{prefix}{k.group(1)}"', svg)
    svg = re.sub(r"url\(#([^)\s]+)\)", lambda k: f"url(#{prefix}{k.group(1)})" if k.group(1) in ids else k.group(0), svg)
    return re.sub(r'(href=")#([^"]+)"', lambda k: f'{k.group(1)}#{prefix}{k.group(2)}"' if k.group(2) in ids else k.group(0), svg)


def render_dot(src: str, ctx: Ctx, line: int, derived: str | None = None) -> str:
    """A ```dot fence -> <figure class="gv">: toolbar, inline SVG, hidden source (the byte-for-byte <pre><code>).
    `derived` is the html of the `text expected` block the graph was generated from (outviz): the figure then
    has Graph / Output tabs holding that block unchanged, and no source panel."""
    ctx.gv += 1
    prefix = f"gv{ctx.gv}-"
    ctx.used_ids.add(prefix)
    nm = re.search(r'^\s*(?:strict\s+)?(?:di)?graph\s+("(?:[^"\\]|\\.)*"|[\w.\-]+)', src, re.I | re.M)
    name = re.sub(r"[^\w.\-]+", "-", nm.group(1).strip('"')).strip("-") if nm else ""
    name = name or f"diagram-{ctx.gv}"
    svg = finish_svg(dot_svg(src, f"docs/{ctx.md_name}:{line}", line, derived is not None), prefix, f"Diagram: {name.replace('_', ' ')}")

    def ico(path: str, cls: str = "gv-ico") -> str:
        return f'<svg class="{cls}" viewBox="0 0 16 16" aria-hidden="true" focusable="false"><path d="{path}"/></svg>'

    btns = []
    for act, label, path in GV_BUTTONS:
        extra = ico(GV_CLOSE_ICON, "gv-ico gv-ico-close") if act == "expand" else ""
        btns.append(f'<button class="gv-btn" type="button" data-act="{act}" aria-label="{label}" title="{label}">{ico(path)}{extra}</button>')
    hint = "drag to pan &middot; ctrl/&#8984;+scroll to zoom &middot; click a node to pin"
    if derived is not None:
        def tab(view: str, label: str) -> str:
            sel = view == "graph"
            return (f'<button class="gv-tab" type="button" role="tab" id="{prefix}tab-{view}" aria-controls="{prefix}panel-{view}" '
                    f'aria-selected="{str(sel).lower()}" data-view="{view}"{"" if sel else ' tabindex="-1"'}>{label}</button>')

        return (f'<figure class="gv gv-derived" data-name="{esc(name)}" data-view="graph">'
                f'<div class="gv-bar"><div class="gv-tabs" role="tablist" aria-label="Show this output as">{tab("graph", "Graph")}{tab("output", "Output")}</div>'
                f'<span class="gv-hint">{hint}</span><span class="spacer"></span>'
                f'<span class="gv-ctl" role="toolbar" aria-label="Diagram controls">{"".join(btns)}</span></div>'
                f'<div class="gv-panel" id="{prefix}panel-graph" role="tabpanel" aria-labelledby="{prefix}tab-graph"><div class="gv-view">{svg}</div></div>'
                f'<div class="gv-panel gv-out" id="{prefix}panel-output" role="tabpanel" aria-labelledby="{prefix}tab-output" hidden>{derived}</div>'
                f'<div class="gv-caption">Graph drawn from this command&rsquo;s output; the Output tab has the original text.</div></figure>')
    # order: zoom out, zoom in, fit, expand, Source, download
    btns.insert(len(btns) - 1, '<button class="gv-btn gv-btn-text" type="button" data-act="source" aria-pressed="false" '
                               'aria-label="Show dot source" title="Show dot source">Source</button>')
    lines = src.count("\n") + 1 if src else 0
    return (f'<figure class="gv" data-name="{esc(name)}">'
            f'<div class="gv-bar" role="toolbar" aria-label="Diagram controls"><span class="lang">diagram</span>'
            f'<span class="gv-hint">{hint}</span>'
            f'<span class="spacer"></span>{"".join(btns)}</div>'
            f'<div class="gv-view">{svg}</div>'
            f'<div class="gv-src" hidden><div class="code" data-lines="{lines}"><div class="code-bar"><span class="lang">dot</span>'
            f'<span class="spacer"></span><button class="copy" type="button" aria-label="Copy code">Copy</button></div>'
            f'<pre><code class="language-dot">{esc(src)}</code></pre></div></div></figure>')


def render_code(info: str, text: str, ctx: Ctx, fold_expected: bool = True, line: int = 0) -> str:
    words = info.split()
    lang = words[0] if words else ""
    flags = words[1:]
    if info.strip() == "dot":
        return render_dot(text, ctx, line)
    expected = "expected" in flags
    lines = text.count("\n") + 1 if text else 0
    cls = "code"
    if expected:
        cls += " expected"
    if lines > LONG_CODE_LINES:
        cls += " long"
    label = "recorded output" if expected else (lang or "")
    hl = f"language-{esc(lang)}" if lang and lang not in ("text", "plain") else "nohighlight"
    expand = '<button class="expand" type="button">Expand</button>' if lines > LONG_CODE_LINES else ""
    block = (f'<div class="{cls}" data-lines="{lines}"><div class="code-bar"><span class="lang">{esc(label)}</span>'
             f'<span class="spacer"></span>{expand}<button class="copy" type="button" aria-label="Copy code">Copy</button></div>'
             f'<pre><code class="{hl}">{esc(text)}</code></pre></div>')
    if expected and fold_expected and lines > LONG_EXPECTED_LINES:
        block = (f'<details class="expected-long"><summary>Recorded output <span class="muted">({lines} lines)</span>'
                 f"</summary>{block}</details>")
    cmd = ctx.cmds.get(line) if expected else None
    dot = derive_dot(text, cmd, ctx, line) if cmd is not None else None
    ctx.graph_view = bool(dot)
    return render_dot(dot, ctx, line, derived=block) if dot else block


def derive_dot(output: str, cmd: str, ctx: Ctx, line: int) -> str | None:
    """Graphviz source for a `text expected` block that is a graph in text form, or None (outviz)."""
    try:
        return outviz.to_dot(output, cmd)
    except Exception as e:  # a parser bug must not silently drop the graph
        raise DotError(f"docs/{ctx.md_name}:{line}: outviz parser failed for `{cmd.splitlines()[0][:80]}`: {type(e).__name__}: {e}") from e


def render_flat(items: list, ctx: Ctx) -> str:
    """Render parse() items without section grouping (callouts, list items)."""
    res = []
    for it in items:
        if isinstance(it, str):
            res.append(it)
        elif it[0] == "code":
            res.append(render_code(it[1], it[2], ctx, line=it[3]))
        elif it[0] == "h":
            res.append(f"<h{min(it[1] + 1, 6)}>{inline(it[2], ctx)}</h{min(it[1] + 1, 6)}>")
        elif it[0] == "hr":
            res.append("<hr>")
    return "".join(res)


# --------------------------------------------------------------------------
# page assembly
# --------------------------------------------------------------------------
class Section:
    def __init__(self, sid: str, num: str | None, title_raw: str):
        self.id, self.num, self.title_raw = sid, num, title_raw
        self.keywords: list[str] = []
        self.search_terms: set[str] = set()

    def index(self, text: str):
        self.search_terms.update(re.findall(r"[a-z0-9_:.]+", text.lower()))


def assemble(items: list, ctx: Ctx, part_n: int | None):
    """Group parsed items into sections, with ordinary inline subheadings.
    Returns (html, [Section])."""
    out: list[str] = []
    sections: list[Section] = []
    state = {"sec": False}
    cur_h2_slug = ""

    def close_sec():
        if state["sec"]:
            out.append("</section>")
            state["sec"] = False

    for idx, it in enumerate(items):
        if isinstance(it, str):
            if sections:
                sections[-1].index(html.unescape(re.sub(r"<[^>]+>", " ", it)))
            out.append(it)
            continue
        kind = it[0]
        if kind == "code":
            if sections:
                sections[-1].index(it[2])
            out.append(render_code(it[1], it[2], ctx, line=it[3]))
        elif kind == "hr":
            nxt = items[idx + 1] if idx + 1 < len(items) else None
            if nxt is None or (isinstance(nxt, tuple) and nxt[0] == "h" and nxt[1] <= 2):
                continue
            out.append("<hr>")
        elif kind == "h":
            level, raw = it[1], it[2]
            text = plain(raw)
            if level == 1:
                hid = f"part-{part_n}" if part_n else ctx.uid("top")
                ctx.used_ids.add(hid)
                out.append(f'<h1 class="page-title" id="{hid}">{inline(raw, ctx)}</h1>@@PAGER_TOP@@')
            elif level == 2:
                close_sec()
                sm = re.match(r"^Section (\d+)\.(\d+) — (.*)$", text)
                if sm:
                    sid = ctx.uid(f"s{sm.group(1)}-{sm.group(2)}")
                    num = f"{sm.group(1)}.{sm.group(2)}"
                else:
                    sid = ctx.uid(f"p{part_n}-{slug(text)}" if part_n else f"h-{slug(text)}")
                    num = None
                cur_h2_slug = sid
                sec = Section(sid, num, raw)
                sections.append(sec)
                done = (f'<label class="done" title="Mark this section as done"><input type="checkbox" data-sec="{num}"> '
                        f"<span>done</span></label>") if num else ""
                out.append(f'<section class="sec" id="{sid}" data-sec="{num or ""}"><div class="sec-head">'
                           f'<h2><a class="anchor" href="#{sid}" aria-label="Link to this section">#</a>{inline(raw, ctx)}</h2>{done}</div>')
                state["sec"] = True
            elif level == 3:
                hid = ctx.uid(f"{cur_h2_slug}-{slug(text)}")
                if sections:
                    sections[-1].keywords.append(text)
                out.append(f'<h3 id="{hid}">{inline(raw, ctx)}</h3>')
            else:
                hid = ctx.uid(f"{cur_h2_slug}-{slug(text)}" if cur_h2_slug else slug(text))
                if sections:
                    sections[-1].keywords.append(text)
                out.append(f'<h{level} id="{hid}"><a class="anchor" href="#{hid}" aria-label="Link to this heading">#</a>{inline(raw, ctx)}</h{level}>')
    close_sec()
    return "".join(out), sections


# --------------------------------------------------------------------------
# site model
# --------------------------------------------------------------------------
def load_progress_seed() -> dict[str, bool]:
    p = DOCS / "PROGRESS.md"
    seed: dict[str, bool] = {}
    if p.exists():
        for m in re.finditer(r"^- \[([ xX])\] (\d+\.\d+) —", p.read_text(), re.M):
            if m.group(1) in "xX":
                seed[m.group(2)] = True
    return seed


def build() -> int:
    files = part_files()
    parts_map = {f.name: part_no(f) for f in files}
    pages = []  # dicts
    for f in files:
        n = part_no(f)
        md = f.read_text()
        ctx = Ctx(f.name, False, parts_map)
        ctx.cmds = outviz.expected_commands(md)
        items = parse(md.split("\n"), ctx, top=True)
        body, secs = assemble(items, ctx, n)
        h1 = re.search(r"^# (.+?)\s*$", md, re.M)
        title = h1.group(1) if h1 else f.name
        pages.append({"n": n, "file": f"part_{n}.html", "title": title, "short": re.sub(r"^Part \d+ — ", "", title),
                      "body": body, "sections": secs, "md": f.name})

    home_md = (DOCS / "README.md").read_text()
    hctx = Ctx("README.md", True, parts_map)
    hctx.cmds = outviz.expected_commands(home_md)
    hitems = parse(home_md.split("\n"), hctx, top=True)
    home_body, home_secs = assemble(hitems, hctx, None)
    h1 = re.search(r"^# (.+?)\s*$", home_md, re.M)
    home_title = h1.group(1) if h1 else "Clang JIT Lab"

    seed = load_progress_seed()
    all_nums = [s.num for p in pages for s in p["sections"] if s.num]

    # ---- sidebar (identical on every page; the active part is marked by JS)
    def sidebar_html(current: str) -> str:
        t = ['<div class="sb-top"><a class="brand" href="index.html">Clang JIT Lab</a>'
             '<button id="sb-collapse" class="sb-collapse icon-btn" type="button" aria-label="Collapse sidebar" aria-expanded="true" '
             'aria-controls="sidebar" title="Collapse sidebar ( [ )">&laquo;</button>'
             '<button class="sb-close icon-btn" type="button" aria-label="Close menu">&times;</button></div>',
             '<div class="sb-search"><input id="filter" type="search" placeholder="Filter sections ( / )" autocomplete="off" '
             'spellcheck="false" aria-label="Filter sections"></div>',
             '<div class="sb-progress" title="Sections marked done"><div class="bar"><div class="fill" id="overall-fill"></div></div>'
             f'<div class="ptext"><span id="overall-text">0 / {len(all_nums)}</span> sections done'
             '<button id="reset-progress" class="link-btn" type="button">reset</button></div></div>',
             '<div class="tree-head"><span>Parts</span><span class="tree-tools">'
             '<button id="parts-expand" class="link-btn" type="button">expand all</button>'
             '<button id="parts-collapse" class="link-btn" type="button">collapse all</button></span></div>',
             '<nav id="tree" aria-label="Lab contents">',
             f'<a class="home{" active" if current == "index.html" else ""}" href="index.html">Home &mdash; overview</a>']
        for p in pages:
            nums = [s.num for s in p["sections"] if s.num]
            t.append(f'<div class="part" data-part="{p["n"]}" data-text="{esc(plain(p["title"]).lower())}">'
                     f'<div class="part-row"><button class="part-toggle" type="button" aria-expanded="false" '
                     f'aria-label="Expand Part {p["n"]}"><span class="chev"></span></button>'
                     f'<a class="part-link" href="{p["file"]}"><span class="pnum">{p["n"]}</span>'
                     f'<span class="ptitle">{esc(p["short"])}</span></a>'
                     f'<span class="pcount" data-nums="{",".join(nums)}">0/{len(nums)}</span></div><ul class="secs">')
            for s in p["sections"]:
                label = plain(s.title_raw)
                m = re.match(r"^Section (\d+\.\d+) — (.*)$", label)
                shown = f'<span class="secnum">{m.group(1)}</span> {inline(re.sub(r"^Section \d+\.\d+ — ", "", s.title_raw), Ctx("", False, {}))}' if m \
                    else inline(s.title_raw, Ctx("", False, {}))
                kw = " ".join([label] + s.keywords + sorted(s.search_terms)).lower()
                t.append(f'<li data-id="{s.id}" data-sec="{s.num or ""}" data-text="{esc(kw)}">'
                         f'<a href="{p["file"]}#{s.id}"><span class="tick"></span><span class="lbl">{shown}</span></a></li>')
            t.append("</ul></div>")
        t.append('</nav><div id="no-results" hidden>No sections match.</div>')
        t.append('<div class="sb-foot">Source: <code>docs/*.md</code> &middot; regenerate with <code>python3 scripts/build_site.py</code></div>')
        return "".join(t)

    # ---- rail: what the sidebar collapses to (desktop/tablet). The ring on a part button is done/total; the
    # seed gives the first paint, app.js refreshes ring + tooltip from localStorage.
    def rail_html(current: str) -> str:
        here = ' aria-current="page"'
        t = ['<nav class="rail" id="rail" aria-label="Parts">',
             '<button id="rail-expand" class="rail-btn" type="button" aria-label="Expand sidebar" aria-expanded="false" '
             'aria-controls="sidebar" title="Expand sidebar ( [ )">&raquo;</button>',
             f'<a class="rail-btn rail-home" href="index.html" aria-label="Home: overview" title="Home &mdash; overview"'
             f'{here if current == "index.html" else ""}>&#8962;</a>',
             '<span class="rail-sep"></span>']
        for p in pages:
            nums = [s.num for s in p["sections"] if s.num]
            done = sum(1 for n in nums if seed.get(n))
            name = esc(plain(p["title"]))
            t.append(f'<a class="rail-btn rail-part" href="{p["file"]}" data-nums="{",".join(nums)}" data-name="{name}" '
                     f'style="--p:{round(100 * done / len(nums)) if nums else 0}%" title="{name} &middot; {done}/{len(nums)} done" '
                     f'aria-label="{name}, {done} of {len(nums)} sections done"{here if current == p["file"] else ""}>'
                     f'<span>{p["n"]}</span></a>')
        t.append("</nav>")
        return "".join(t)

    def pager(i: int | None) -> str:
        """Prev/next navigation; i is the index into pages, None for home."""
        if i is None:
            return ""
        prev = pages[i - 1] if i > 0 else None
        nxt = pages[i + 1] if i + 1 < len(pages) else None
        left = (f'<a class="pg prev" href="{prev["file"]}"><span class="dir">&larr; Previous</span>'
                f'<span class="name">{esc(prev["title"])}</span></a>') if prev else \
            '<a class="pg prev" href="index.html"><span class="dir">&larr; Back</span><span class="name">Overview</span></a>'
        right = (f'<a class="pg next" href="{nxt["file"]}"><span class="dir">Next &rarr;</span>'
                 f'<span class="name">{esc(nxt["title"])}</span></a>') if nxt else \
            '<a class="pg next" href="index.html"><span class="dir">Finished &rarr;</span><span class="name">Overview</span></a>'
        return f'<nav class="pager" aria-label="Part navigation">{left}{right}</nav>'

    def page(fname: str, title: str, body: str, idx: int | None, extra_top: str = "") -> str:
        data = json.dumps({"seed": seed, "nums": all_nums, "page": fname})
        body = body.replace("@@PAGER_TOP@@", pager(idx) if idx is not None else "")
        bottom = pager(idx) if idx is not None else ""
        return f"""<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(plain(title))} &middot; Clang JIT Lab</title>
<meta name="color-scheme" content="dark">
<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<script>
/* apply persisted sidebar state before first paint */
(function(){{try{{if(localStorage.getItem('clangjit.v2.sidebar')==='collapsed')document.documentElement.classList.add('sb-collapsed');}}catch(e){{}}}})();
</script>
<link rel="stylesheet" href="assets/style.css?v={ASSET_VERSION}">
</head>
<body>
<header class="topbar">
  <button id="menu" class="icon-btn" type="button" aria-label="Toggle navigation" aria-controls="sidebar"><span class="burger"></span></button>
  <a class="brand-top" href="index.html">Clang JIT Lab</a>
  <span class="crumb" id="crumb">{esc(plain(title))}</span>
  <span class="spacer"></span>
  <span class="top-progress" title="Sections done"><span class="bar"><span class="fill" id="top-fill"></span></span><span id="top-text"></span></span>
</header>
<div class="layout">
<aside id="sidebar" aria-label="Navigation"><div class="sb-full" id="sb-full" tabindex="0" role="region" aria-label="Scrollable lesson navigation">{sidebar_html(fname)}</div>{rail_html(fname)}</aside>
<div id="backdrop"></div>
<main id="content"><article class="doc">{extra_top}{body}{bottom}</article>
<footer class="foot">Generated from <code>docs/{'README.md' if idx is None else pages[idx]['md']}</code> by <code>scripts/build_site.py</code>.</footer></main>
</div>
<script id="repllab-data" type="application/json">{data.replace("</", "<\\/")}</script>
<script defer src="assets/vendor/highlight.min.js?v={ASSET_VERSION}"></script>
<script defer src="assets/app.js?v={ASSET_VERSION}"></script>
</body>
</html>
"""

    cards = ['<div class="cards">']
    for p in pages:
        nums = [s.num for s in p["sections"] if s.num]
        cards.append(f'<a class="card" href="{p["file"]}" data-nums="{",".join(nums)}"><span class="cn">Part {p["n"]}</span>'
                     f'<span class="ct">{esc(p["short"])}</span><span class="cbar"><span class="fill"></span></span>'
                     f'<span class="cc">0/{len(nums)} done</span></a>')
    cards.append("</div>")
    home_body = home_body.replace("@@PAGER_TOP@@", "")
    # Explain the engine before presenting the lesson navigation cards.
    home_body = home_body.replace('<section class="sec" id="h-learning-path"',
                                  "".join(cards) + '<section class="sec" id="h-learning-path"', 1)

    # remove only what this script generates (keeps e.g. site/_preview.png)
    for old_f in list(SITE.glob("*.html")) + [SITE / "assets"]:
        if old_f.is_dir():
            shutil.rmtree(old_f, ignore_errors=True)
        elif old_f.exists():
            old_f.unlink()
    (SITE / "assets").mkdir(parents=True, exist_ok=True)
    for a in ("style.css", "app.js", "favicon.svg"):
        shutil.copy(ASSETS / a, SITE / "assets" / a)
    shutil.copytree(ASSETS / "vendor", SITE / "assets" / "vendor")
    (SITE / "index.html").write_text(page("index.html", home_title, home_body, None))
    for i, p in enumerate(pages):
        (SITE / p["file"]).write_text(page(p["file"], p["title"], p["body"], i))

    for w in sorted(set(warnings)):
        print("warning:", w)
    nsec = sum(len(p["sections"]) for p in pages)
    print(f"build_site: {len(pages)} parts, {nsec} sections ({len(all_nums)} trackable) -> {SITE.relative_to(ROOT)}/index.html")
    return 0


def lambda_safe(s: str) -> str:
    """Escape backslashes for use as a re.sub replacement string."""
    return s.replace("\\", "\\\\")


if __name__ == "__main__":
    try:
        sys.exit(build())
    except DotError as e:
        sys.exit(f"build_site: error: {e}")
