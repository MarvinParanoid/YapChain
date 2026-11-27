#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic tests of tools/deps/fetch_deps.py against a loopback HTTP server with scripted
failures: retry and backoff, Retry-After, truncated and stalled bodies, corrupted archives (hash
mismatch), conservative 403 handling, non-retryable errors, redirects, size limits, cache reuse and a
tampered cache. Nothing is ever published under the final name unless its SHA-256 matches, and no
temporary file is left behind. Also checks that the manifest agrees with the pins in
cmake/Dependencies.cmake, cmake/Protobuf.cmake and scripts/localnet/localnet.py."""
import hashlib
import http.server
import os
import random
import re
import shutil
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "deps"))
import fetch_deps as fd  # noqa: E402

PAYLOAD = bytes(random.Random(7).getrandbits(8) for _ in range(200_000))
SHA = hashlib.sha256(PAYLOAD).hexdigest()
fd.DEPS["test"] = ("test-archive.tar.gz", SHA, "https://invalid.example/unused", ())


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    hits = {}
    lock = threading.Lock()

    def log_message(self, *a):
        pass

    def _count(self):
        with Handler.lock:
            Handler.hits[self.path] = Handler.hits.get(self.path, 0) + 1
            return Handler.hits[self.path]

    def _ok(self, body=PAYLOAD, length=None, cut=None):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        self.end_headers()
        self.wfile.write(body if cut is None else body[:cut])
        self.wfile.flush()
        if cut is not None or length is not None:
            self.close_connection = True

    def _err(self, code, extra=None):
        self.send_response(code)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):  # noqa: N802
        n = self._count()
        p = self.path
        if p == "/ok":
            return self._ok()
        if p == "/flaky500":
            return self._err(500) if n <= 2 else self._ok()
        if p == "/retryafter":
            return self._err(503, {"Retry-After": "7"}) if n == 1 else self._ok()
        if p == "/truncated-once":
            return self._ok(cut=len(PAYLOAD) // 2) if n == 1 else self._ok()
        if p == "/truncated-always":
            return self._ok(cut=len(PAYLOAD) // 3)
        if p == "/corrupt":
            bad = bytearray(PAYLOAD)
            bad[1234] ^= 0x01
            return self._ok(bytes(bad))
        if p == "/short-no-length":  # no Content-Length, fewer bytes: only the hash can catch it
            self.send_response(200)
            self.end_headers()
            self.wfile.write(PAYLOAD[:1000])
            self.close_connection = True
            return None
        if p == "/403-once":
            return self._err(403) if n == 1 else self._ok()
        if p == "/403-always":
            return self._err(403)
        if p == "/404":
            return self._err(404)
        if p == "/stall":
            self.send_response(200)
            self.send_header("Content-Length", str(len(PAYLOAD)))
            self.end_headers()
            self.wfile.write(PAYLOAD[:100])
            self.wfile.flush()
            time.sleep(1.5)
            self.close_connection = True
            return None
        if p == "/redirect-ok":
            return self._err(302, {"Location": f"http://127.0.0.1:{self.server.server_port}/ok"})
        if p == "/redirect-external-http":
            return self._err(302, {"Location": "http://invalid.example/x"})
        if p == "/big":
            return self._ok(length=fd.MAX_BYTES + 1, cut=0)
        return self._err(404)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


def run_case(name, check):
    try:
        ok, detail = check()
    except Exception as e:  # noqa: BLE001 — a test harness reports every unexpected exception
        ok, detail = False, f"unexpected {type(e).__name__}: {e}"
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")
    return ok


def main():
    srv = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"
    work = tempfile.mkdtemp(prefix="fetch-deps-test-")
    final_name = fd.DEPS["test"][0]
    results = []

    def fresh():
        d = tempfile.mkdtemp(dir=work)
        return d

    def leftovers(d):
        return [f for f in os.listdir(d) if f.startswith(".") and ".part-" in f]

    def attempt(path, d=None, **kw):
        d = d or fresh()
        sleeps = []
        kw.setdefault("attempts", 4)
        kw.setdefault("timeout", 0.5)
        try:
            out = fd.fetch("test", d, url=base + path, sleep=sleeps.append, log=lambda m: None,
                           rng=random.Random(1), **kw)
            err = None
        except fd.FetchError as e:
            out, err = None, str(e)
        published = os.path.exists(os.path.join(d, final_name))
        good = published and fd.sha256_file(os.path.join(d, final_name)) == SHA
        return {"out": out, "err": err, "sleeps": sleeps, "published": published, "good": good,
                "hits": Handler.hits.get(path, 0), "left": leftovers(d), "dir": d}

    def case(name, path, expect_ok, hits, **kw):
        def check():
            Handler.hits.pop(path, None)
            r = attempt(path, **kw)
            ok = (r["good"] if expect_ok else (not r["published"] and r["err"] is not None)) \
                and r["hits"] == hits and not r["left"]
            if not expect_ok and r["published"]:
                ok = False
            return ok, f"hits={r['hits']} sleeps={[round(s, 2) for s in r['sleeps']]} err={r['err']}"
        results.append(run_case(name, check))

    case("plain download", "/ok", True, 1)
    case("two HTTP 500 then success (backoff)", "/flaky500", True, 3)
    case("redirect within loopback http", "/redirect-ok", True, 1)
    case("truncated body is retried", "/truncated-once", True, 2)
    case("always truncated: bounded attempts, nothing published", "/truncated-always", False, 4)
    case("corrupted archive: hash mismatch stops at once", "/corrupt", False, 1)
    case("short body without Content-Length: hash mismatch", "/short-no-length", False, 1)
    case("403 once, retried once (default)", "/403-once", True, 2)
    case("403 always: reported after one retry", "/403-always", False, 2)
    case("403 with --retry-403 0: no retry", "/403-always", False, 1, retry_403=0)
    case("404 is not retried", "/404", False, 1)
    case("stalled body: read timeout, bounded attempts", "/stall", False, 2, attempts=2, timeout=0.3)
    case("redirect to external plain http is refused", "/redirect-external-http", False, 1)
    case("body over the size limit is refused", "/big", False, 1)

    def retry_after():
        Handler.hits.pop("/retryafter", None)
        r = attempt("/retryafter")
        return r["good"] and r["sleeps"] and r["sleeps"][0] >= 7, f"sleeps={r['sleeps']}"
    results.append(run_case("Retry-After honoured", retry_after))

    def backoff_shape():
        Handler.hits.pop("/truncated-always", None)
        r = attempt("/truncated-always", attempts=5, backoff_base=2.0, backoff_max=5.0)
        s = r["sleeps"]
        # attempt k waits in [d/2, d] with d = min(5, 2 * 2^(k-1)): 2, 4, 5, 5
        bounds = [(1, 2), (2, 4), (2.5, 5), (2.5, 5)]
        ok = len(s) == 4 and all(lo <= x <= hi for x, (lo, hi) in zip(s, bounds))
        return ok, f"sleeps={[round(x, 2) for x in s]}"
    results.append(run_case("exponential backoff with jitter, capped", backoff_shape))

    def deadline():
        Handler.hits.pop("/flaky500", None)
        r = attempt("/flaky500", deadline=0.5, backoff_base=2.0)
        return (not r["published"] and r["err"] and "deadline" in r["err"]), f"err={r['err']}"
    results.append(run_case("overall deadline stops the retries", deadline))

    def cache_reuse():
        d = fresh()
        attempt("/ok", d)
        Handler.hits.pop("/ok", None)
        r = attempt("/ok", d)
        return r["good"] and r["hits"] == 0, f"hits={r['hits']}"
    results.append(run_case("verified cache is reused without a request", cache_reuse))

    def cache_tampered():
        d = fresh()
        p = os.path.join(d, final_name)
        with open(p, "wb") as f:
            f.write(PAYLOAD[:-1] + b"x")
        before = fd.sha256_file(p)
        Handler.hits.pop("/ok", None)
        r = attempt("/ok", d)
        ok = r["err"] and "cached archive" in r["err"] and fd.sha256_file(p) == before and r["hits"] == 0
        return ok, f"err={r['err']}"
    results.append(run_case("tampered cache entry is reported, not replaced", cache_tampered))

    def offline_missing():
        r = attempt("/ok", offline=True)
        return (r["err"] and "offline" in r["err"] and not r["published"]), f"err={r['err']}"
    results.append(run_case("--offline without a cache entry fails", offline_missing))

    def offline_readonly():
        d = fresh()
        attempt("/ok", d)
        os.remove(os.path.join(d, ".fetch-deps.lock"))
        os.chmod(d, 0o555)
        try:
            r = attempt("/ok", d, offline=True)
            ok_good = r["good"] and r["err"] is None and not os.path.exists(os.path.join(d, ".fetch-deps.lock"))
        finally:
            os.chmod(d, 0o755)
        p = os.path.join(d, final_name)
        with open(p, "r+b") as f:
            f.seek(10)
            f.write(b"\0")
        r2 = attempt("/ok", d, offline=True)
        return ok_good and r2["err"] is not None and "not used" in r2["err"], f"err={r2['err']}"
    results.append(run_case("--offline verifies a read-only directory and rejects a bad archive", offline_readonly))

    def https_only():
        try:
            fd.fetch("test", fresh(), url="http://invalid.example/x", log=lambda m: None)
        except fd.FetchError as e:
            return "https" in str(e), str(e)
        return False, "accepted a plain http URL"
    results.append(run_case("non-loopback plain http is refused", https_only))

    def manifest_matches_pins():
        text = ""
        for f in ("cmake/Dependencies.cmake", "cmake/Protobuf.cmake"):
            with open(os.path.join(ROOT, f)) as fh:
                text += fh.read()
        vars_ = dict(re.findall(r"set\((YAP_\w+)\s+([^\s)]+)\)", text))
        def expand(s):
            return re.sub(r"\$\{(YAP_\w+)\}", lambda m: vars_[m.group(1)], s)
        pins = {}
        for m in re.finditer(r'yap_dep_url\(\w+\s+"([^"]+)"\s+"([^"]+)"\)', text):
            pins[expand(m.group(1))] = expand(m.group(2))
        for m in re.finditer(r'_yap_fetch_archive\(\w+\s+"([^"]+)"\s+"([^"]+)"\s+\$\{(\w+)\}', text):
            pins[expand(m.group(1))] = expand(m.group(2))
        shas = set(v for k, v in vars_.items() if k.endswith("_SHA256"))
        with open(os.path.join(ROOT, "scripts", "localnet", "localnet.py")) as fh:
            ln = fh.read()
        problems = []
        for name, (file_name, sha, url, groups) in fd.DEPS.items():
            if name == "test":
                continue
            if name == "cometbft":
                if f'"{file_name}"' not in ln or f'"{sha}"' not in ln or f'"{url}"' not in ln:
                    problems.append("cometbft differs from localnet.py")
                continue
            if pins.get(file_name) != url:
                problems.append(f"{file_name}: url {url} vs cmake {pins.get(file_name)}")
            if sha not in shas:
                problems.append(f"{file_name}: sha256 not pinned in cmake")
        missing = set(pins) - {v[0] for v in fd.DEPS.values()}
        if missing:
            problems.append(f"pinned in cmake but not in the manifest: {sorted(missing)}")
        return not problems, (problems or f"{len(pins)} cmake pins + cometbft agree")
    results.append(run_case("manifest agrees with the cmake and localnet pins", manifest_matches_pins))

    srv.shutdown()
    shutil.rmtree(work, ignore_errors=True)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
