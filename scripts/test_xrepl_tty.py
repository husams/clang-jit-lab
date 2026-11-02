#!/usr/bin/env python3
"""Tab completion needs a terminal; test it through a pseudo-terminal (Section 7.7).

    scripts/test_xrepl_tty.py [path/to/p07_xrepl]

Types keys into a real tty session of xrepl and checks what the session did with them:
  1. "int foo = 1;", then "fo<Tab>" + Enter must evaluate `foo`        -> "(int) 1"
  2. "%ty<Tab> 6 * 7" + Enter must run %type                           -> "type     : int"
Exit status: the number of failed checks (0 if both hold).
"""
import os
import pty
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "bin", "p07_xrepl")


def session(keys):
    master, slave = pty.openpty()
    env = dict(os.environ, TERM="xterm", XREPL_HISTORY=os.path.join(ROOT, "out", "p07_tty_history"))
    proc = subprocess.Popen([BIN], stdin=slave, stdout=slave, stderr=slave, close_fds=True, env=env, cwd=ROOT)
    os.close(slave)
    out = b""

    def drain(seconds):
        """Read until `seconds` pass, or until output arrived and then stayed quiet for 0.6 s."""
        nonlocal out
        end = time.time() + seconds
        last = None
        while time.time() < end:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    out += os.read(master, 65536)
                except OSError:
                    return
                last = time.time()
            elif last is not None and time.time() - last > 0.6:
                return

    drain(2)
    for k in keys:
        os.write(master, k.encode())
        drain(2.5 if "\t" in k or k.endswith("\n") else 0.3)
    os.write(master, b"%quit\n")
    drain(1)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    return out.decode(errors="replace")


checks = [
    ("identifier completion", ["int foo = 1;\n", "fo\t", "\n"], "(int) 1"),
    ("% command completion", ["%ty\t", " 6 * 7\n"], "type     : int"),
]
failed = 0
for name, keys, want in checks:
    ok = want in session(keys)
    print(("PASS     tty: " if ok else "FAIL     tty: ") + name)
    failed += not ok
sys.exit(failed)
