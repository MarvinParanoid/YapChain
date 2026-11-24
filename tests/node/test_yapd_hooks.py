#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The production yapd contains no test hooks; only the separate yapd-test-hooks binary does (M2.4f).

    test_yapd_hooks.py <yapd> <yapd-test-hooks>

1. The string YAPD_TEST_CRASH and the hook messages are absent from the yapd binary, present in the
   test binary.
2. `yapd --version` does not announce a test build; `yapd-test-hooks --version` does.
3. Behaviour: yapd started with YAPD_TEST_CRASH set (even to an invalid value, which the test build
   refuses at start-up) starts, serves its socket, ignores the variable and stops cleanly on SIGTERM.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

MARKERS = [b"YAPD_TEST_CRASH", b"SIGKILL at ", b"TEST BUILD"]


def fail(msg):
    print(f"FAILED: {msg}", file=sys.stderr)
    sys.exit(1)


def main(yapd, hooks):
    prod = open(yapd, "rb").read()
    test = open(hooks, "rb").read()
    for m in MARKERS:
        if m in prod:
            fail(f"yapd contains test-hook code ({m.decode()})")
        if m not in test:
            fail(f"yapd-test-hooks lacks {m.decode()}")

    env = dict(os.environ, YAPD_TEST_CRASH="before_commit:1")
    v = subprocess.run([yapd, "--version"], capture_output=True, text=True, env=env)
    if v.returncode != 0 or "TEST BUILD" in v.stdout or not v.stdout.startswith("yapd "):
        fail(f"yapd --version: {v.returncode} {v.stdout!r}")
    v = subprocess.run([hooks, "--version"], capture_output=True, text=True)
    if v.returncode != 0 or "TEST BUILD" not in v.stdout:
        fail(f"yapd-test-hooks --version: {v.returncode} {v.stdout!r}")

    d = tempfile.mkdtemp(prefix="yh-")
    try:
        # The test build refuses an invalid value at start-up (exit 1) ...
        bad = dict(os.environ, YAPD_TEST_CRASH="nonsense")
        r = subprocess.run([hooks, "--home", os.path.join(d, "h")], capture_output=True, text=True, env=bad,
                           timeout=30)
        if r.returncode != 1 or "YAPD_TEST_CRASH" not in r.stderr:
            fail(f"yapd-test-hooks with an invalid YAPD_TEST_CRASH: {r.returncode} {r.stderr!r}")
        # ... the production build does not look at it at all.
        sock = os.path.join(d, "p", "yapd.sock")
        p = subprocess.Popen([yapd, "--home", os.path.join(d, "p")], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=bad)
        deadline = time.time() + 30
        while not os.path.exists(sock) and p.poll() is None and time.time() < deadline:
            time.sleep(0.05)
        if not os.path.exists(sock):
            p.kill()
            fail(f"yapd with YAPD_TEST_CRASH set did not start: {p.communicate()[0]!r}")
        p.send_signal(signal.SIGTERM)
        out = p.communicate(timeout=30)[0]
        if p.returncode != 0 or "YAPD_TEST_CRASH" in out or "TEST BUILD" in out:
            fail(f"yapd with YAPD_TEST_CRASH set: exit {p.returncode}, log {out!r}")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("yapd has no test hooks; yapd-test-hooks has them")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
