# SPDX-License-Identifier: Apache-2.0
"""Filesystem-independent oracle for the ABCI Unix socket of a yapd container (M2.7.1).

A replaced socket is recognised by behaviour, never by inode or timestamps (file systems such as ext4
reuse a freed inode number at once; timestamp resolution varies):

- stale:     the path exists, is a Unix socket, and connect() fails with ECONNREFUSED (nobody listens);
- answering: a fresh connection to the same path gets the exact ABCI Echo answer for a random nonce,
             which only a real ABCI server produces (an unrelated listener cannot echo the nonce).

Inode and ctime are only reported (`describe`) for diagnostics. All waits use monotonic deadlines.
No Docker and no privileges are needed, so tests/docker/test_socket_oracle.py checks this module alone.
"""
import errno
import os
import secrets
import socket
import stat
import time


class SocketOracleError(AssertionError):
    pass


def _uvarint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _field(tag, payload):
    return bytes([tag]) + _uvarint(len(payload)) + payload


def echo_request(message):
    """Length-delimited Request{echo{message}} (Request.echo = field 1, RequestEcho.message = field 1)."""
    body = _field(0x0A, _field(0x0A, message) if message else b"")  # proto3 omits an empty string
    return _uvarint(len(body)) + body


def echo_response(message):
    """The exact length-delimited Response{echo{message}} (Response.echo = field 2)."""
    body = _field(0x12, _field(0x0A, message) if message else b"")
    return _uvarint(len(body)) + body


def describe(path, stat_fn=os.lstat):
    """Diagnostics only: type, inode and ctime of the path (never used for a decision)."""
    try:
        st = stat_fn(path)
    except FileNotFoundError:
        return {"exists": False}
    return {"exists": True, "socket": stat.S_ISSOCK(st.st_mode), "inode": st.st_ino, "ctime_ns": st.st_ctime_ns}


def connect_errno(path, timeout=2.0):
    """0 if a connection to the Unix socket succeeds, otherwise the errno of connect()."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        return 0
    except OSError as e:
        return e.errno or -1
    finally:
        s.close()


def check_stale(path, stat_fn=os.lstat):
    """The leftover path exists, is a Unix socket and nobody listens on it (ECONNREFUSED).
    Returns the diagnostics; raises SocketOracleError otherwise."""
    d = describe(path, stat_fn)
    if not d["exists"]:
        raise SocketOracleError(f"{path}: no leftover socket file")
    if not d["socket"]:
        raise SocketOracleError(f"{path}: exists but is not a Unix socket")
    e = connect_errno(path)
    if e != errno.ECONNREFUSED:
        what = "a listener accepted the connection" if e == 0 else f"connect failed with {errno.errorcode.get(e, e)}"
        raise SocketOracleError(f"{path}: not stale: {what} (expected ECONNREFUSED)")
    return d


def abci_echo(path, timeout=2.0):
    """One fresh connection; send Echo with a random nonce and require the exact answer before a monotonic
    deadline. Returns (ok, detail)."""
    nonce = secrets.token_hex(16).encode()
    want = echo_response(nonce)
    deadline = time.monotonic() + timeout
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.settimeout(timeout)
        try:
            s.connect(path)
        except OSError as e:
            return False, f"connect: {errno.errorcode.get(e.errno, e.errno)}"
        try:
            s.sendall(echo_request(nonce))
            got = b""
            while len(got) < len(want):
                left = deadline - time.monotonic()
                if left <= 0:
                    return False, f"no complete Echo answer within {timeout}s (got {got.hex() or 'nothing'})"
                s.settimeout(left)
                chunk = s.recv(len(want) - len(got))
                if not chunk:
                    return False, f"connection closed after {got.hex() or 'nothing'}"
                got += chunk
                if not want.startswith(got):
                    return False, f"wrong Echo answer {got.hex()} (expected {want.hex()})"
        except socket.timeout:
            return False, f"no complete Echo answer within {timeout}s (got {got.hex() or 'nothing'})"
        except OSError as e:
            return False, f"connection failed during the exchange: {e}"
        if got != want:
            return False, f"wrong Echo answer {got.hex()} (expected {want.hex()})"
        return True, f"Echo answered on a fresh connection (nonce {nonce.decode()[:8]}…)"
    finally:
        s.close()


def wait_answering(path, timeout, poll=0.2, attempt_timeout=2.0):
    """Wait (monotonic deadline) until a fresh connection to `path` gets the exact Echo answer.
    Returns the detail of the successful exchange; raises SocketOracleError with the last failure."""
    deadline = time.monotonic() + timeout
    last = "not tried"
    while True:
        ok, last = abci_echo(path, timeout=min(attempt_timeout, max(0.05, deadline - time.monotonic())))
        if ok:
            return last
        if time.monotonic() >= deadline:
            raise SocketOracleError(f"{path}: no valid ABCI Echo answer within {timeout}s: {last}")
        time.sleep(min(poll, max(0.0, deadline - time.monotonic())))


def pid_gone(pid):
    """True once no process with this pid exists (from the caller's pid namespace)."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    return False
