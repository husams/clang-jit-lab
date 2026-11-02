#!/usr/bin/env python3
"""Replay a file of clang-repl inputs and print a transcript, as if typed.

    scripts/session.py manifests/p01_first.repl
    scripts/session.py manifests/p02_flags.repl -- --Xcc=-std=c++20
    printf '%s\n' 'int x = 1;' 'x' | scripts/session.py -

Why this exists: when clang-repl reads a pipe, its two output streams are
block-buffered independently (value printing goes through llvm::outs(), your
printf/std::cout through C stdio), so `cat file | clang-repl` can print the
results out of order (Section 1.7 shows that). This script gives clang-repl a
pseudo-terminal for stdout/stderr, which makes both streams unbuffered or
line-buffered, and feeds stdin one input at a time. After each input it sends
an unknown %-command as a sentinel and waits for clang-repl's error message
about it, so every line of output is attributed to the input that produced it.
The sentinel is not a code input, so it never creates a partial translation
unit and `%undo` is unaffected.

Blank lines in the file are skipped. (Typed at clang-repl's prompt, an empty
line is an input of its own, which `%undo` would count.)

Output format:
    clang-repl> <input line>          one per input line
    clang-repl...   <continued line>  lines after a trailing backslash
    <whatever clang-repl printed for that input>

Options (before the file):
    --keep-addrs   do not mask addresses (0x + 7 or more hex digits -> 0xADDR,
                   and (void *)<7+ digits>ULL -> (void *)ADDR)
    --no-echo      print only clang-repl's output, no prompt lines
                   (ORC symbol sets such as `{ (main, { b, a }) }` are always sorted:
                   ORC prints them in hash order)
    --prompt NAME  name shown in the echoed prompt (default clang-repl): "NAME> input", "NAME...   more"
    --repl PATH    clang-repl binary (default: $LLVM/bin/clang-repl,
                   LLVM defaults to /opt/homebrew/opt/llvm)
    --timeout N    seconds to wait for one input (default 120)
Everything after `--` is passed to clang-repl.
"""
import os
import pty
import re
import select
import subprocess
import sys
import time

ADDR = re.compile(r"0x[0-9a-fA-F]{7,}")
DEC_PTR = re.compile(r"\(void \*\)\d{7,}ULL")  # pointers baked into synthesized code (-ast-print)


def usage(msg):
    sys.stderr.write(f"session.py: {msg}\n")
    sys.stderr.write((__doc__ or "").split("\n\n")[1] + "\n")
    sys.exit(2)


def parse_args(argv):
    opts = {"keep": False, "echo": True, "timeout": 120.0, "prompt": "clang-repl",
            "repl": os.path.join(os.environ.get("LLVM", "/opt/homebrew/opt/llvm"), "bin", "clang-repl")}
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    files = []
    it = iter(argv)
    for a in it:
        if a == "--keep-addrs":
            opts["keep"] = True
        elif a == "--no-echo":
            opts["echo"] = False
        elif a == "--prompt":
            opts["prompt"] = next(it, None) or usage("--prompt needs a name")
        elif a == "--repl":
            opts["repl"] = next(it, None) or usage("--repl needs a path")
        elif a == "--timeout":
            opts["timeout"] = float(next(it, "") or usage("--timeout needs seconds"))
        elif a.startswith("--") and a != "-":
            usage(f"unknown option {a}")
        else:
            files.append(a)
    if len(files) != 1:
        usage("expected exactly one input file (or - for stdin)")
    return opts, files[0], extra


def logical_inputs(lines):
    """Group physical lines into inputs: a line ending in a backslash continues."""
    cur = []
    for raw in lines:
        line = raw.rstrip("\n")
        if not cur and not line.strip():
            continue  # blank lines are not inputs
        cur.append(line)
        if not line.rstrip().endswith("\\"):
            yield cur
            cur = []
    if cur:
        yield cur


class Reader:
    """Collects everything clang-repl writes to the pty."""

    def __init__(self, fd):
        self.fd, self.buf, self.eof = fd, b"", False

    def pump(self, timeout):
        if self.eof:
            return
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return
        try:
            data = os.read(self.fd, 65536)
        except OSError:  # EIO: the slave side is closed (process exited)
            data = b""
        if data:
            self.buf += data
        else:
            self.eof = True

    def until(self, needle, deadline):
        while needle not in self.buf and not self.eof and time.time() < deadline:
            self.pump(0.05)
        return needle in self.buf

    def drain(self, deadline):
        while not self.eof and time.time() < deadline:
            self.pump(0.05)

    def take(self, n=None):
        n = len(self.buf) if n is None else n
        out, self.buf = self.buf[:n], self.buf[n:]
        return out


# ORC prints symbol sets in hash order, e.g. `{ (main, { b, a }) }`; sort them so transcripts are stable.
SYMSET = re.compile(r"\{ \(([^,{}]+), \{ ([^{}]*) \}\) \}")


def sort_symsets(text):
    return SYMSET.sub(lambda m: "{ (%s, { %s }) }" % (m.group(1), ", ".join(sorted(m.group(2).split(", ")))), text)


def clean(data, keep):
    text = data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "")
    text = sort_symsets(text)
    return text if keep else DEC_PTR.sub("(void *)ADDR", ADDR.sub("0xADDR", text))


def main(argv):
    opts, path, extra = parse_args(argv)
    src = sys.stdin.read() if path == "-" else open(path).read()
    inputs = list(logical_inputs(src.split("\n")))

    master, slave = pty.openpty()
    env = dict(os.environ, TERM="dumb")  # no colored diagnostics
    try:
        proc = subprocess.Popen([opts["repl"], *extra], stdin=subprocess.PIPE, stdout=slave, stderr=slave,
                                close_fds=True, env=env)
    except OSError as e:
        sys.exit(f"session.py: cannot run {opts['repl']}: {e}")
    os.close(slave)
    assert proc.stdin is not None
    stdin = proc.stdin
    rd = Reader(master)
    out = sys.stdout

    def emit(text):
        out.write(text)
        out.flush()

    def sync(n):
        """Send sentinel n; return what clang-repl printed before its reply (None on EOF/timeout)."""
        try:
            stdin.write(f"%__sync_{n}__\n".encode())
            stdin.flush()
        except BrokenPipeError:
            return None
        marker = f'error: Invalid % command "%__sync_{n}__"'.encode()
        if not rd.until(marker, time.time() + opts["timeout"]):
            return None
        before = rd.take(rd.buf.index(marker))
        # drop the sentinel's own message: `error: ... list commands` followed by two line ends
        # (the message ends in "\n" and logAllUnhandledErrors adds one; the pty turns each into "\r\n")
        tail = re.compile(rb"list commands\r?\n\r?\n")
        deadline = time.time() + 5
        while not tail.search(rd.buf) and not rd.eof and time.time() < deadline:
            rd.pump(0.05)
        m = tail.search(rd.buf)
        rd.take(m.end() if m else None)
        return before

    # Output printed while clang-repl starts (before the first input) is shown first, unprompted.
    startup = sync(0)
    if startup is None:
        emit(clean(rd.take(), opts["keep"]))
        inputs = []
    elif startup:
        emit(clean(startup, opts["keep"]))

    for n, group in enumerate(inputs, 1):
        if opts["echo"]:
            for i, line in enumerate(group):
                emit((opts["prompt"] + "> " if i == 0 else opts["prompt"] + "...   ") + line + "\n")
        joined = "\n".join(group) + "\n"
        quitting = "".join(l.rstrip("\\").strip() for l in group) == "%quit"
        try:
            stdin.write(joined.encode())
            stdin.flush()
        except BrokenPipeError:
            break
        if quitting:
            break
        before = sync(n)
        if before is None:
            emit(clean(rd.take(), opts["keep"]))
            if not rd.eof:
                emit(f"[session.py: no response within {opts['timeout']:.0f}s]\n")
            break
        emit(clean(before, opts["keep"]))

    try:
        stdin.close()
    except BrokenPipeError:
        pass
    deadline = time.time() + opts["timeout"]
    while proc.poll() is None and time.time() < deadline:
        rd.pump(0.05)
    rd.drain(time.time() + 1)
    # clang-repl's line editor prints one newline when it stops reading; it is noise in a transcript
    final = clean(rd.take(), opts["keep"])
    emit(final[1:] if final.startswith("\n") else final)
    if proc.poll() is None:
        proc.kill()
        emit("[session.py: clang-repl did not exit; killed]\n")
    rc = proc.wait()
    if rc < 0:
        import signal
        emit(f"[clang-repl terminated by {signal.Signals(-rc).name}]\n")
    elif rc:
        emit(f"[clang-repl exit status {rc}]\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BrokenPipeError:  # e.g. `scripts/session.py x.repl | head`: stop quietly
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        sys.exit(0)
