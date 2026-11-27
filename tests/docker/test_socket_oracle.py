#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regression tests of the stale ABCI socket oracle (tests/docker/abci_socket.py, M2.7.1), without Docker or
privileges: real Unix sockets in a temporary directory and small in-process servers.

    test_socket_oracle.py

The oracle must accept a replaced socket even when the file system reuses the inode number (and ctime),
reject a socket that stays stale, reject listeners that are not an ABCI server, and never accept a path
merely because some process is running.
"""
import errno
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abci_socket as o  # noqa: E402

FAILS = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        FAILS.append(msg)


def raises(fn, *args, **kw):
    try:
        fn(*args, **kw)
    except o.SocketOracleError as e:
        return str(e)
    return None


def read_uvarint(conn):
    n, shift = 0, 0
    while True:
        b = conn.recv(1)
        if not b:
            raise EOFError
        n |= (b[0] & 0x7F) << shift
        if not b[0] & 0x80:
            return n
        shift += 7


def recv_exact(conn, n):
    out = b""
    while len(out) < n:
        c = conn.recv(n - len(out))
        if not c:
            raise EOFError
        out += c
    return out


class Server:
    """A listener on `path`. mode: echo (a correct ABCI Echo responder), canned (answers 02 12 00 — an
    empty Echo — to anything), silent (accepts, never answers), close (accepts and closes), http."""

    def __init__(self, path, mode):
        self.path, self.mode = path, mode
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(8)
        self.stop = False
        self.conns = []
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def loop(self):
        self.sock.settimeout(0.1)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            self.conns.append(c)
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            if self.mode == "close":
                c.close()
                return
            if self.mode == "silent":
                return
            if self.mode == "http":
                c.recv(64)
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
                return
            body = recv_exact(c, read_uvarint(c))
            if self.mode == "canned":
                c.sendall(bytes([0x02, 0x12, 0x00]))
                return
            # echo: Request{echo(1){message(1)}} -> Response{echo(2){message(1)}}
            assert body[0] == 0x0A
            inner = body[2:] if body[1] < 0x80 else None
            assert inner is not None and inner[0] == 0x0A
            msg = inner[2:2 + inner[1]]
            c.sendall(o.echo_response(msg))
        except (EOFError, OSError, AssertionError):
            pass

    def close(self, unlink=False):
        self.stop = True
        self.t.join(2)
        self.sock.close()
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass
        if unlink:
            os.unlink(self.path)


def leave_stale(path):
    """What a SIGKILLed server leaves behind: a bound socket file without a listener."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    s.listen(1)
    s.close()


def replace_like_yapd(path, mode="echo"):
    """yapd's Server::start: a stale socket (connect refused) is unlinked, then bound again."""
    if os.path.exists(path):
        assert o.connect_errno(path) == errno.ECONNREFUSED
        os.unlink(path)
    return Server(path, mode)


FakeStat = namedtuple("FakeStat", "st_mode st_ino st_ctime_ns")


def same_inode_stat(real=os.lstat):
    """A stat that reports one fixed inode number and ctime for every call, as a file system that reuses the
    freed inode at once (and has coarse timestamps) would."""
    def f(path):
        st = real(path)
        return FakeStat(st.st_mode, 4007045, 1_795_736_837_000_000_000)
    return f


def main():
    with tempfile.TemporaryDirectory(prefix="yap-sock-") as tmp:
        p = os.path.join(tmp, "yapd.sock")

        # --- stale detection
        check(raises(o.check_stale, p) and "no leftover" in raises(o.check_stale, p), "missing path is not 'stale'")
        with open(p, "w") as f:
            f.write("x")
        check("not a Unix socket" in (raises(o.check_stale, p) or ""), "regular file is not 'stale'")
        os.unlink(p)
        leave_stale(p)
        d = o.check_stale(p)
        check(d["socket"] and o.connect_errno(p) == errno.ECONNREFUSED, "leftover socket: ECONNREFUSED, accepted as stale")
        srv = replace_like_yapd(p)
        check("not stale" in (raises(o.check_stale, p) or ""), "a live listener is never 'stale'")

        # --- replacement accepted, also when inode and ctime are reused
        detail = o.wait_answering(p, 5)
        check("Echo answered" in detail, "replaced socket answers the nonce Echo")
        fake = same_inode_stat()
        srv.close(unlink=False)                     # the server dies: its file stays behind, stale
        before = o.check_stale(p, stat_fn=fake)
        srv = replace_like_yapd(p)
        after = o.describe(p, stat_fn=fake)
        check(before["inode"] == after["inode"] and before["ctime_ns"] == after["ctime_ns"],
              "simulated file system reports the same inode and ctime before and after")
        check("Echo answered" in o.wait_answering(p, 5), "replacement accepted although inode and ctime are reused")
        srv.close(unlink=True)

        # --- a socket that stays stale is rejected, within the monotonic deadline
        leave_stale(p)
        t0 = time.monotonic()
        err = raises(o.wait_answering, p, 1.0)
        took = time.monotonic() - t0
        check(err is not None and "ECONNREFUSED" in err, f"stale socket rejected: {err}")
        check(took < 3.0, f"deadline respected ({took:.2f}s for a 1s deadline)")
        os.unlink(p)

        # --- unrelated listeners are rejected
        for mode, why in (("canned", "wrong Echo answer"), ("silent", "no complete Echo answer"),
                          ("close", "connection"), ("http", "wrong Echo answer")):
            srv = Server(p, mode)
            err = raises(o.wait_answering, p, 1.0, attempt_timeout=0.3)
            check(err is not None and why in err, f"unrelated listener ({mode}) rejected: {err}")
            srv.close(unlink=True)

        # --- a running process is not enough
        leave_stale(p)
        proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            check(not o.pid_gone(proc.pid), "a running process is reported alive")
            err = raises(o.wait_answering, p, 1.0)
            check(err is not None, f"a running process with a stale socket is rejected: {err}")
        finally:
            proc.kill()
            proc.wait()
        check(o.pid_gone(proc.pid), "a killed and reaped process is reported gone")
        os.unlink(p)
        srv = Server(p, "silent")
        err = raises(o.wait_answering, p, 1.0, attempt_timeout=0.3)
        check(err is not None, f"an accepting but silent server is rejected: {err}")
        srv.close(unlink=True)

        # --- framing
        check(o.echo_request(b"") == bytes([0x02, 0x0A, 0x00]), "empty Echo request matches yap-abci-probe bytes")
        check(o.echo_response(b"") == bytes([0x02, 0x12, 0x00]), "empty Echo response matches yap-abci-probe bytes")
        long = b"x" * 200
        check(o.echo_request(long)[:2] == bytes([0xCE, 0x01]), "lengths over 127 use a multi-byte uvarint")

    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("socket oracle: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
