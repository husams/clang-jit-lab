#!/usr/bin/env python3
"""Verify the generated site (site/) against docs/*.md.

  * site/index.html and site/part_N.html exist for every part file
  * every part H1 and every `## Section N.M` heading has its anchor (id) and
    its title text in the HTML; every other h2/h3/h4 heading has an id
  * every fenced code block in the markdown appears, byte for byte and in
    order, as a <pre><code> block in the HTML (expected output, and the source
    of every ```dot diagram)
  * every ```dot fence became exactly one <figure class="gv"> holding an inline
    <svg class="gv-svg"> and a toolbar whose buttons all have an aria-label
  * every `text expected` block that scripts/outviz turns into a graph became
    one <figure class="gv gv-derived"> with the inline svg, Graph / Output tabs
    wired to their panels, and the unchanged expected-output block
  * table and callout counts match the markdown
  * every internal href (page, page#fragment, #fragment) resolves to an
    existing file and an existing id
  * the HTML is well formed (balanced tags), has a viewport meta, a sidebar
    listing all parts and sections, and prev/next navigation

Usage: scripts/check_site.py           (run scripts/build_site.py first)
"""
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SITE = ROOT / "site"
problems: list[str] = []
checks = 0


def err(msg):
    problems.append(msg)


VOID = {"meta", "link", "br", "hr", "img", "input", "area", "base", "col", "embed", "source", "track", "wbr"}


class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.dup_ids: list[str] = []
        self.hrefs: list[str] = []
        self.script_sources: list[str] = []
        self.codes: list[str] = []  # text of every <pre><code>
        self.tables = 0
        self.callouts = 0
        self.stack: list[str] = []
        self.bad: list[str] = []
        self._in_pre_code = False
        self._buf: list[str] = []
        self.heading_text: dict[str, str] = {}  # id -> text for <section>/<h3>/<h4>
        self._cur_sec = None
        self._sec_h2 = False
        self._h2_buf: list[str] = []
        self.sidebar_links: list[str] = []
        self._in_sidebar = 0
        self.pager = 0
        self.meta_viewport = False
        self.sec_titles: dict[str, str] = {}
        self.gv_figs = 0  # <figure class="gv">
        self.gv_svgs = 0  # ... of which hold the diagram <svg class="gv-svg">
        self.gv_bad_buttons = 0  # toolbar buttons without an aria-label
        self._gv_at: int | None = None  # stack depth of the open figure.gv
        self._gv_svg = False
        self.gv_derived = 0  # <figure class="gv gv-derived"> (graph views of `text expected` blocks)
        self.gv_derived_svgs = 0  # ... with the svg
        self.gv_derived_out = 0  # ... with the expected-output block
        self.gv_bad_tabs = 0  # ... without exactly two tabs and two tabpanels
        self.aria_refs: list[str | None] = []  # ids named by aria-controls / aria-labelledby of tabs and panels
        self._gv_derived = False
        self._gv_out = False
        self._gv_tabs = self._gv_panels = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script" and a.get("src"):
            self.script_sources.append(a["src"].split("?", 1)[0])
        if "id" in a:
            if a["id"] in self.ids:
                self.dup_ids.append(a["id"])
            self.ids.add(a["id"])
        if tag == "a" and "href" in a:
            self.hrefs.append(a["href"])
            if self._in_sidebar:
                self.sidebar_links.append(a["href"])
        if tag == "meta" and a.get("name") == "viewport":
            self.meta_viewport = True
        if tag == "aside" and a.get("id") == "sidebar":
            self._in_sidebar = 1
        cls = (a.get("class") or "").split()
        if tag == "figure" and "gv" in cls:
            self._gv_at, self._gv_svg, self._gv_out = len(self.stack), False, False
            self._gv_derived, self._gv_tabs, self._gv_panels = "gv-derived" in cls, 0, 0
            if self._gv_derived:
                self.gv_derived += 1
            else:
                self.gv_figs += 1
        if self._gv_at is not None:
            if tag == "svg" and "gv-svg" in cls:
                self._gv_svg = True
            if tag == "div" and "code" in cls and "expected" in cls:
                self._gv_out = True
            if a.get("role") == "tab":
                self._gv_tabs += 1
                self.aria_refs.append(a.get("aria-controls"))
            if a.get("role") == "tabpanel":
                self._gv_panels += 1
                self.aria_refs.append(a.get("aria-labelledby"))
        if tag == "button" and "gv-btn" in cls and not a.get("aria-label"):
            self.gv_bad_buttons += 1
        if tag == "table":
            self.tables += 1
        if tag in ("details", "div") and "callout" in (a.get("class") or "").split():
            self.callouts += 1
        if tag == "nav" and a.get("class") == "pager":
            self.pager += 1
        if tag == "code" and self.stack and self.stack[-1] == "pre":
            self._in_pre_code = True
            self._buf = []
        if tag == "section" and "sec" in (a.get("class") or "").split():
            self._cur_sec = a.get("id")
        if tag == "h2" and self._cur_sec:
            self._sec_h2 = True
            self._h2_buf = []
        if tag not in VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.bad.append(f"</{tag}> closes <{self.stack[-1] if self.stack else None}>")
            if tag in self.stack:
                while self.stack and self.stack.pop() != tag:
                    pass
            return
        self.stack.pop()
        if tag == "figure" and self._gv_at == len(self.stack):
            if self._gv_derived:
                self.gv_derived_svgs += self._gv_svg
                self.gv_derived_out += self._gv_out
                self.gv_bad_tabs += (self._gv_tabs, self._gv_panels) != (2, 2)
            else:
                self.gv_svgs += self._gv_svg
            self._gv_at = None
        if tag == "code" and self._in_pre_code:
            self.codes.append("".join(self._buf))
            self._in_pre_code = False
        if tag == "aside":
            self._in_sidebar = 0
        if tag == "h2" and self._sec_h2:
            self._sec_h2 = False
            self.sec_titles[self._cur_sec] = re.sub(r"^#", "", "".join(self._h2_buf)).strip()

    def handle_data(self, data):
        if self._in_pre_code:
            self._buf.append(data)
        if self._sec_h2:
            self._h2_buf.append(data)


def parse_page(path: Path) -> Page:
    p = Page()
    p.feed(path.read_text())
    p.close()
    return p


def md_fence_blocks(text: str) -> list[tuple[str, str]]:
    """(info string, contents) of every fenced block (also inside > blockquotes / lists), in order."""
    out, lines, i = [], text.split("\n"), 0
    while i < len(lines):
        m = re.match(r"^(\s*(?:>\s?)*)(`{3,}|~{3,})\s*(.*?)\s*$", lines[i])
        if not m:
            i += 1
            continue
        prefix, fence, info = m.group(1), m.group(2), m.group(3)
        indent = len(re.sub(r">\s?", "", prefix))
        closer = re.compile(r"^\s*(?:>\s?)*" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}\s*$")
        body = []
        i += 1
        while i < len(lines) and not closer.match(lines[i]):
            ln = re.sub(r"^\s*>\s?", "", lines[i]) if prefix.strip().startswith(">") else lines[i]
            body.append(ln[indent:] if ln[:indent].strip() == "" else ln)
            i += 1
        i += 1
        out.append((info, "\n".join(body)))
    return out


def md_fences(text: str) -> list[str]:
    return [body for _, body in md_fence_blocks(text)]


def md_dot_count(text: str) -> int:
    return sum(1 for info, _ in md_fence_blocks(text) if info == "dot")


def derived_count(name: str, md: str) -> int:
    """How many `text expected` blocks of this markdown outviz draws as a graph (the same call build_site makes)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import outviz

    n = 0
    for line, cmd, out in outviz.expected_pairs(md):
        try:
            n += outviz.to_dot(out, cmd) is not None
        except Exception as e:
            err(f"{name}: docs line {line}: outviz parser failed: {type(e).__name__}: {e}")
    return n


def check_diagrams(name: str, pg: Page, md: str) -> None:
    """Every ```dot fence is one figure.gv with an inline diagram svg, every graph-shaped `text expected` block
    one figure.gv-derived with the svg, two wired tabs and its output block; all toolbar buttons are labelled."""
    global checks
    want = md_dot_count(md)
    checks += 6
    if not (want == pg.gv_figs == pg.gv_svgs):
        err(f"{name}: {want} ```dot fences in markdown, {pg.gv_figs} figure.gv, {pg.gv_svgs} with an inline <svg class=\"gv-svg\">")
    if pg.gv_bad_buttons:
        err(f"{name}: {pg.gv_bad_buttons} diagram toolbar button(s) without aria-label")
    drawn = derived_count(name, md)
    if not (drawn == pg.gv_derived == pg.gv_derived_svgs == pg.gv_derived_out):
        err(f"{name}: {drawn} graph-shaped outputs in markdown, {pg.gv_derived} figure.gv-derived, "
            f"{pg.gv_derived_svgs} with the svg, {pg.gv_derived_out} with the output block")
    if pg.gv_bad_tabs:
        err(f"{name}: {pg.gv_bad_tabs} graph view(s) without exactly two tabs and two tabpanels")
    missing = [r for r in pg.aria_refs if r not in pg.ids]
    if missing:
        err(f"{name}: tab/tabpanel aria references to missing ids: {missing[:3]}")


def md_nofence(text: str) -> str:
    lines, keep, fence = text.split("\n"), [], None
    for ln in lines:
        m = re.match(r"^\s*(?:>\s?)*(`{3,}|~{3,})", ln)
        if fence is None and m:
            fence = m.group(1)
            continue
        if fence is not None:
            if re.match(r"^\s*(?:>\s?)*" + re.escape(fence[0]) + "{" + str(len(fence)) + r",}\s*$", ln):
                fence = None
            continue
        keep.append(ln)
    return "\n".join(keep)


def main() -> int:
    global checks
    if not (SITE / "index.html").exists():
        print("check_site: site/ missing; run python3 scripts/build_site.py first")
        return 1
    parts = sorted(DOCS.glob("part_[0-9]*_*.md"), key=lambda p: int(re.match(r"part_(\d+)_", p.name).group(1)))
    pages: dict[str, Page] = {"index.html": parse_page(SITE / "index.html")}
    n_sections = n_code = n_tables = n_callouts = n_diagrams = n_derived = 0
    first_sidebar = None

    for f in parts:
        n = int(re.match(r"part_(\d+)_", f.name).group(1))
        name = f"part_{n}.html"
        path = SITE / name
        if not path.exists():
            err(f"missing {name}")
            continue
        pg = parse_page(path)
        pages[name] = pg
        md = f.read_text()
        bare = md_nofence(md)

        checks += 1
        if "&lt;!-- runnable-example:" in path.read_text() or "&lt;!-- /runnable-example" in path.read_text():
            err(f"{name}: authoring marker leaked into the visible lesson")

        checks += 1
        if f"part-{n}" not in pg.ids:
            err(f"{name}: no id part-{n} (H1)")
        h1 = re.search(r"^# (.+?)\s*$", md, re.M).group(1)
        # section anchors
        for num, title in re.findall(r"^## Section (\d+\.\d+) — (.+?)\s*$", bare, flags=re.M):
            checks += 1
            sid = "s" + num.replace(".", "-")
            n_sections += 1
            if sid not in pg.ids:
                err(f"{name}: missing section anchor #{sid} ({num})")
                continue
            want = re.sub(r"`", "", f"Section {num} — {title}")
            got = pg.sec_titles.get(sid, "")
            if re.sub(r"\s+", " ", got) != re.sub(r"\s+", " ", want):
                err(f"{name}: #{sid} heading text differs: {got!r} vs {want!r}")
        # other headings: each must exist as an id-bearing element => count check
        n_md_heads = len(re.findall(r"^#{2,6} ", bare, flags=re.M))
        n_html_heads = len(re.findall(r'<(?:section class="sec"|h[3-6]) id="', path.read_text()))
        checks += 1
        if n_md_heads != n_html_heads:
            err(f"{name}: {n_md_heads} h2-h6 headings in markdown but {n_html_heads} anchored headings in HTML")
        # code blocks: exact, ordered
        want_codes = md_fences(md)
        checks += 1
        n_code += len(want_codes)
        if len(want_codes) != len(pg.codes):
            err(f"{name}: {len(want_codes)} fenced blocks in markdown, {len(pg.codes)} <pre><code> in HTML")
        else:
            for k, (w, g) in enumerate(zip(want_codes, pg.codes), 1):
                if w != g:
                    first = next((i for i, (x, y) in enumerate(zip(w.split("\n"), g.split("\n"))) if x != y), 0)
                    err(f"{name}: code block #{k} differs from markdown (first differing line {first + 1})")
                    break
        n_diagrams += pg.gv_svgs
        n_derived += pg.gv_derived_svgs
        check_diagrams(name, pg, md)
        # tables
        md_tables = len(re.findall(r"^\s*\|?\s*:?-{2,}:?\s*(?:\|\s*:?-{2,}:?\s*)+\|?\s*$", bare, flags=re.M))
        checks += 1
        n_tables += pg.tables
        if md_tables != pg.tables:
            err(f"{name}: {md_tables} tables in markdown, {pg.tables} in HTML")
        # callouts
        md_callouts = len(re.findall(r"^>\s*\[!\w+\]", bare, flags=re.M))
        checks += 1
        n_callouts += pg.callouts
        if md_callouts != pg.callouts:
            err(f"{name}: {md_callouts} callouts in markdown, {pg.callouts} in HTML")
        # structure
        checks += 3
        if pg.bad:
            err(f"{name}: malformed HTML: {pg.bad[:3]}")
        if pg.stack:
            err(f"{name}: unclosed tags: {pg.stack[:5]}")
        if pg.dup_ids:
            err(f"{name}: duplicate ids: {sorted(set(pg.dup_ids))[:5]}")
        if not pg.meta_viewport:
            err(f"{name}: no viewport meta")
        checks += 1
        if pg.pager != 2:
            err(f"{name}: expected prev/next navigation at top and bottom, found {pg.pager}")
        # sidebar lists every section of this part
        side = set(pg.sidebar_links)
        for num, _ in re.findall(r"^## Section (\d+\.\d+) — (.+?)\s*$", bare, flags=re.M):
            checks += 1
            if f"{name}#s{num.replace('.', '-')}" not in side:
                err(f"{name}: sidebar lacks link to {num}")
        if first_sidebar is None:
            first_sidebar = side

    # index
    ip = pages["index.html"]
    checks += 2
    if ip.bad or ip.stack:
        err(f"index.html: malformed HTML {ip.bad[:3]} {ip.stack[:3]}")
    for f in parts:
        n = int(re.match(r"part_(\d+)_", f.name).group(1))
        checks += 1
        if f"part_{n}.html" not in ip.hrefs:
            err(f"index.html: no link to part_{n}.html")
    home_nofence = md_nofence((DOCS / "README.md").read_text())
    want_h = len(re.findall(r"^## ", home_nofence, flags=re.M))
    got_h = len(re.findall(r'<section class="sec" id="', (SITE / "index.html").read_text()))
    checks += 1
    if want_h != got_h:
        err(f"index.html: {want_h} h2+ headings in README.md, {got_h} sections")
    checks += 1
    want_codes = md_fences((DOCS / "README.md").read_text())
    if want_codes != ip.codes:
        err("index.html: code blocks differ from docs/README.md")
    n_diagrams += ip.gv_svgs
    n_derived += ip.gv_derived_svgs
    check_diagrams("index.html", ip, (DOCS / "README.md").read_text())

    # links: every href must resolve
    n_links = 0
    for name, pg in pages.items():
        checks += 1
        if ("assets/app.js" not in pg.script_sources
                or "assets/vendor/highlight.min.js" not in pg.script_sources
                or pg.script_sources.index("assets/vendor/highlight.min.js")
                > pg.script_sources.index("assets/app.js")):
            err(f"{name}: local highlighter must load before app.js")
        for h in pg.hrefs:
            if re.match(r"^(https?:|mailto:)", h):
                continue
            n_links += 1
            checks += 1
            path, _, frag = h.partition("#")
            tgt = name if not path else path
            if not (SITE / tgt).exists():
                err(f"{name}: broken link {h}")
                continue
            if frag:
                tp = pages.get(tgt) or parse_page(SITE / tgt)
                if unquote(frag) not in tp.ids:
                    err(f"{name}: link {h} -> no id #{frag} in {tgt}")

    # assets referenced
    for a in ("assets/style.css", "assets/app.js", "assets/vendor/highlight.min.js", "assets/vendor/LICENSE"):
        checks += 1
        if not (SITE / a).exists():
            err(f"missing {a}")

    print(f"check_site: {len(pages)} pages, {n_sections} sections, {n_code} code blocks ({n_diagrams} diagrams, {n_derived} graph views), {n_tables} tables, "
          f"{n_callouts} callouts, {n_links} internal links, {checks} checks, {len(problems)} problem(s)")
    for p in problems:
        print("  " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
