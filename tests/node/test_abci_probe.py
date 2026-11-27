#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""yap-abci-probe against the real yapd and against servers that must NOT count as healthy (M2.7).

    test_abci_probe.py <yap-abci-probe> <yapd>

1. usage errors exit 64; a missing socket exits 1;
2. a stale socket file (no listener) is "not answering";
3. yapd started on that stale socket path replaces it and answers; a readiness wait (--wait-ms)
   succeeds; 30 probes in a row leave yapd healthy (connections are released);
4. a server that answers something else, and one that accepts but never answers, are not healthy
   (the second within the timeout);
5. after SIGTERM yapd exits 0, removes its socket, and the probe fails again.
"""
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def fail(msg):
    print(f"FAILED: {msg}", file=sys.stderr)
    sys.exit(1)


def probe(binary, *args, timeout=30):
    r = subprocess.run([binary, *args], capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def fake_server(path, answer):
    """A Unix server that reads the request and replies `answer` (None: never replies)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(4)
    stop = threading.Event()

    def loop():
        s.settimeout(0.2)
        held = []
        while not stop.is_set():
            try:
                c, _ = s.accept()
            except (socket.timeout, OSError):
                continue
            c.recv(16)
            if answer is not None:
                c.sendall(answer)
                c.close()
            else:
                held.append(c)
        for c in held:
            c.close()
    t = threading.Thread(target=loop, daemon=True)
    t.start()

    def close():
        stop.set()
        t.join()
        s.close()
        os.unlink(path)
    return close


def main(probe_bin, yapd):
    base = tempfile.mkdtemp(prefix="probe-", dir="/tmp")
    try:
        rc, _ = probe(probe_bin)
        if rc != 64:
            fail(f"no arguments: exit {rc}, expected 64")
        rc, _ = probe(probe_bin, "--socket", "/x", "--timeout-ms", "abc")
        if rc != 64:
            fail(f"bad --timeout-ms: exit {rc}, expected 64")
        sock = os.path.join(base, "yapd.sock")
        rc, out = probe(probe_bin, "--socket", sock)
        if rc != 1 or "No such file" not in out:
            fail(f"missing socket: exit {rc}: {out}")

        # 2. stale socket: bound, then the listener is gone (the file stays)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(sock)
        s.close()
        rc, out = probe(probe_bin, "--socket", sock)
        if rc != 1 or "refused" not in out:
            fail(f"stale socket: exit {rc}: {out}")
        print("ok   missing and stale sockets are not healthy")

        # 3. yapd on the stale path; readiness wait
        home = os.path.join(base, "home")
        log = open(os.path.join(base, "yapd.log"), "w")
        p = subprocess.Popen([yapd, "--home", home, "--socket", sock], stdout=log, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        rc, out = probe(probe_bin, "--socket", sock, "--wait-ms", "20000")
        if rc != 0:
            p.kill()
            fail(f"yapd not ready: exit {rc}: {out}")
        print(f"ok   yapd replaced the stale socket and answers ({time.monotonic() - t0:.2f} s)")
        for i in range(30):
            rc, out = probe(probe_bin, "--socket", sock, "--quiet")
            if rc != 0 or out:
                p.kill()
                fail(f"probe {i}: exit {rc}: {out!r}")
        if p.poll() is not None:
            fail("yapd exited during probing")
        print("ok   30 consecutive probes, yapd still running")

        # 4. servers that must not pass
        other = os.path.join(base, "other.sock")
        close = fake_server(other, b"\x02\x0a\x00")  # echoes the request, which is not a Response
        rc, out = probe(probe_bin, "--socket", other)
        close()
        if rc != 1 or "unexpected answer" not in out:
            fail(f"wrong answer accepted: exit {rc}: {out}")
        close = fake_server(other, None)
        t0 = time.monotonic()
        rc, out = probe(probe_bin, "--socket", other, "--timeout-ms", "500")
        took = time.monotonic() - t0
        close()
        if rc != 1 or "no answer" not in out or took > 3:
            fail(f"silent server: exit {rc} after {took:.2f}s: {out}")
        print(f"ok   wrong answer and silent server are not healthy (timeout honoured: {took:.2f} s)")

        # 5. shutdown
        p.send_signal(signal.SIGTERM)
        try:
            code = p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            fail("yapd did not stop on SIGTERM")
        if code != 0 or os.path.exists(sock):
            fail(f"yapd exit {code}, socket left behind: {os.path.exists(sock)}")
        rc, _ = probe(probe_bin, "--socket", sock)
        if rc != 1:
            fail(f"probe after shutdown: exit {rc}")
        print("ok   SIGTERM: exit 0, socket removed, probe fails")
    finally:
        shutil.rmtree(base, ignore_errors=True)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    sys.exit(main(sys.argv[1], sys.argv[2]))
