"""Graph views for `text expected` blocks.

Some recorded outputs *are* graphs in text form (a `debug.DumpCFG` dump, a
one-line-per-block summary, a dominator tree, a table of edges). The site
builder asks this package for a Graphviz source for such an output and, when it
gets one, shows the output as an interactive diagram with the original text
one click away (see render_dot in scripts/build_site.py).

Each parser module exposes

    PARSERS: list[tuple[re.Pattern, Callable[[str, str], str | None]]]

The regex is searched in the text of the bash command that produced the
output; the function gets (output_text, command_text) and returns a complete
`digraph` that follows the diagram contract of docs/AUTHORING.md (no colors or
fonts, meaning only in `class=`), or None when there is nothing worth drawing.
Modules are tried in the order below, and only if their file exists; the first
non-None result wins. A parser that raises stops the build, naming the block.

    to_dot(text, cmd)            -> dot source or None
    expected_commands(md_text)   -> {line of a `text expected` fence: its command}
                                    (the pairing scripts/doccheck.py uses)

Environment: OUTVIZ_MODULES=cfg,p02 restricts (and orders) the modules, for
debugging a single parser.
"""
import importlib
import os
import sys
from pathlib import Path

MODULES: list[str] = []  # no output parsers in this lab; to_dot() always returns None
_HERE = Path(__file__).resolve().parent
_loaded: list | None = None


def _modules() -> list:
    global _loaded
    if _loaded is None:
        names = os.environ.get("OUTVIZ_MODULES")
        wanted = [n for n in names.split(",") if n] if names else MODULES
        _loaded = [importlib.import_module(f"{__name__}.{n}") for n in wanted if (_HERE / f"{n}.py").exists()]
    return _loaded


def to_dot(text: str, cmd: str) -> str | None:
    for mod in _modules():
        for pattern, parse in mod.PARSERS:
            if pattern.search(cmd):
                dot = parse(text, cmd)
                if dot:
                    return dot
    return None


def expected_pairs(md_text: str) -> list[tuple[int, str, str]]:
    """(line of a `text expected` fence, the command it records, the recorded output), paired as doccheck pairs them."""
    scripts = str(_HERE.parent)
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import doccheck

    _, blocks = doccheck.parse(md_text)
    return [(exp["start"] + 1, "\n".join(cmd["lines"]), "\n".join(exp["lines"])) for cmd, exp in doccheck.pairs(blocks)]


def expected_commands(md_text: str) -> dict[int, str]:
    """`text expected` fence line (1-based) -> text of the `bash` block it records."""
    return {line: cmd for line, cmd, _ in expected_pairs(md_text)}
