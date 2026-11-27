#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetches the pinned third-party source archives (ADR-0018, ADR-0027) into a verified cache directory.

Every archive is identified by its exact file name and SHA-256. The cache directory has the layout
YAP_DEPS_DIR expects (cmake -DYAP_DEPS_DIR=<dir> builds offline from it), and
`localnet.py build-cometbft` takes the CometBFT archive from here.

Acquisition rules (M2.7):
  - bounded attempts; a connect/stall timeout per network operation and an overall deadline per
    attempt and per archive;
  - exponential backoff with jitter for retryable failures only: connection errors, timeouts, an
    incomplete body, HTTP 408/425/429/500/502/503/504 (Retry-After is honoured up to the backoff cap);
  - HTTP 403 is treated conservatively: it may be an access restriction, so it is retried at most
    --retry-403 times (default 1) with the same request and then reported. Nothing is changed to get
    around it (no other URL, headers or credentials);
  - other HTTP errors (404, 401, 410, ...) and a SHA-256 mismatch of a complete download stop at once;
  - TLS verification is the Python default (system CAs, host name checked) and cannot be turned off;
    redirects must stay on https;
  - the body is streamed to a private temporary file next to the cache entry, hashed while written,
    fsynced, and published with an atomic rename ONLY if the SHA-256 matches. A mismatching, truncated
    or interrupted download never appears under the final name;
  - an existing cache entry is reused only after its SHA-256 is verified again; a cache entry with a
    wrong hash is an error (it is reported, not replaced: it can only appear through tampering or
    storage corruption).

Usage:
  fetch_deps.py --dest DIR [--group build|test|cometbft|all] [--only FILE ...] [--offline]
                [--attempts N] [--timeout S] [--deadline S] [--retry-403 N]
  fetch_deps.py --list
Exit codes: 0 all archives verified in DIR, 1 usage, 2 acquisition or verification failure.
Standard library only; run with `python3 -I`.
"""
import argparse
import fcntl
import hashlib
import http.client
import os
import random
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# name -> (file name in YAP_DEPS_DIR, sha256, url, groups). The file names and hashes are the ones in
# cmake/Dependencies.cmake, cmake/Protobuf.cmake and scripts/localnet/localnet.py; the test
# tests/ci/test_fetch_deps.py checks that the copies agree.
DEPS = {
    "libsodium": ("libsodium-1.0.22.tar.gz",
                  "adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349",
                  "https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz",
                  ("build",)),
    "lmdb": ("lmdb-0.9.36.tar.gz",
             "7d55c3cbc1ef55f9b0076d3ddb16d0d9996d05b9caa7ca889ce10430ac677b90",
             "https://github.com/LMDB/lmdb/archive/refs/tags/LMDB_0.9.36.tar.gz",
             ("build",)),
    "json": ("json-3.12.0.tar.xz",
             "42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa",
             "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz",
             ("build",)),
    "cli11": ("CLI11-2.7.2.hpp",
              "ffa9a30da295c5858fb5f91f9f45771bab09471d7010a34c7c68c857a330dd76",
              "https://github.com/CLIUtils/CLI11/releases/download/v2.7.2/CLI11.hpp",
              ("build",)),
    "abseil": ("abseil-cpp-20260817.0.tar.gz",
               "f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d",
               "https://github.com/abseil/abseil-cpp/releases/download/20260817.0/abseil-cpp-20260817.0.tar.gz",
               ("build",)),
    "protobuf": ("protobuf-36.2.tar.gz",
                 "3d9642a662d10e68ebae5e53f14dcce5105684212d5078f8e0d47d1ab3ae6b64",
                 "https://github.com/protocolbuffers/protobuf/releases/download/v36.2/protobuf-36.2.tar.gz",
                 ("build",)),
    "catch2": ("catch2-3.16.1.tar.gz",
               "d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d",
               "https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz",
               ("test",)),
    "cometbft": ("cometbft-v0.40.0.tar.gz",
                 "c458c51a452318a849905cdeb1eb1c4437cff0b5412bb6ca5d948c7c37d77a8a",
                 "https://github.com/cometbft/cometbft/archive/refs/tags/v0.40.0.tar.gz",
                 ("cometbft",)),
}

RETRYABLE_HTTP = {408, 425, 429, 500, 502, 503, 504}
MAX_BYTES = 256 << 20  # no pinned archive comes close; a bigger body is not one of ours


class FetchError(RuntimeError):
    """A failure that ends the acquisition of one archive (after any retries)."""


class _Retryable(Exception):
    def __init__(self, msg, delay_hint=None):
        super().__init__(msg)
        self.delay_hint = delay_hint


class _HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):
    """Follows redirects only to https (or to loopback http when the original URL was loopback http,
    which only the tests use)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = urllib.parse.urlsplit(newurl)
        if new.scheme != "https" and not (_is_loopback_http(req.full_url) and _is_loopback_http(newurl)):
            raise FetchError(f"refusing redirect from {req.full_url} to non-https {newurl}")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _is_loopback_http(url):
    u = urllib.parse.urlsplit(url)
    return u.scheme == "http" and u.hostname in ("127.0.0.1", "::1", "localhost")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _fsync_dir(d):
    fd = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _retry_after(headers):
    v = headers.get("Retry-After") if headers is not None else None
    if v is None:
        return None
    try:
        return max(0.0, float(int(v.strip())))
    except ValueError:
        return None  # HTTP-date form: fall back to our own backoff


def _download_once(url, part_path, expected_sha256, timeout, deadline, max_bytes, opener):
    """One attempt: streams url into part_path (already created, empty). Returns on a complete body
    whose SHA-256 matches; raises _Retryable or FetchError otherwise."""
    req = urllib.request.Request(url, headers={"User-Agent": "yapchain-fetch-deps/1"})
    try:
        resp = opener.open(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        hint = _retry_after(e.headers)
        e.close()
        if e.code in RETRYABLE_HTTP:
            raise _Retryable(f"HTTP {e.code} {e.reason}", hint) from None
        if e.code == 403:
            raise _Retryable403(f"HTTP 403 {e.reason}") from None
        raise FetchError(f"HTTP {e.code} {e.reason} (not retried)") from None
    except urllib.error.URLError as e:
        if isinstance(e.reason, FetchError):
            raise e.reason from None
        raise _Retryable(f"connection failed: {e.reason}") from None
    except (TimeoutError, socket.timeout) as e:
        raise _Retryable(f"timeout while connecting: {e}") from None
    except (ConnectionError, http.client.HTTPException, OSError) as e:
        raise _Retryable(f"connection failed: {e}") from None
    with resp:
        length = resp.headers.get("Content-Length")
        expected_len = int(length) if length is not None and length.isdigit() else None
        if expected_len is not None and expected_len > max_bytes:
            raise FetchError(f"Content-Length {expected_len} exceeds the limit of {max_bytes} bytes")
        h = hashlib.sha256()
        got = 0
        with open(part_path, "wb") as f:
            while True:
                if time.monotonic() > deadline:
                    raise _Retryable(f"attempt deadline exceeded after {got} bytes")
                try:
                    chunk = resp.read(1 << 16)
                except http.client.IncompleteRead as e:
                    raise _Retryable(f"incomplete body: {got + len(e.partial)} of {expected_len} bytes") from None
                except (TimeoutError, socket.timeout):
                    raise _Retryable(f"read stalled for {timeout}s after {got} bytes") from None
                except (ConnectionError, http.client.HTTPException, OSError) as e:
                    raise _Retryable(f"connection lost after {got} bytes: {e}") from None
                if not chunk:
                    break
                got += len(chunk)
                if got > max_bytes:
                    raise FetchError(f"body exceeds the limit of {max_bytes} bytes")
                h.update(chunk)
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())
    if expected_len is not None and got != expected_len:
        raise _Retryable(f"incomplete body: {got} of {expected_len} bytes")
    actual = h.hexdigest()
    if actual != expected_sha256:
        # A complete body with the wrong content is not a transient failure: it is reported, not retried.
        raise FetchError(f"SHA-256 mismatch: expected {expected_sha256}, got {actual} ({got} bytes); "
                         "the archive was discarded")


class _Retryable403(_Retryable):
    pass


def fetch(name, dest, *, url=None, attempts=5, timeout=30.0, deadline=900.0, retry_403=1,
          backoff_base=2.0, backoff_max=60.0, max_bytes=MAX_BYTES, offline=False,
          sleep=time.sleep, log=None, rng=None):
    """Makes DEPS[name] available as a verified file in dest and returns its path. url overrides the
    pinned URL (tests only: the hash is never overridable)."""
    file_name, sha, pinned_url, _ = DEPS[name]
    url = url or pinned_url
    if not (urllib.parse.urlsplit(url).scheme == "https" or _is_loopback_http(url)):
        raise FetchError(f"{file_name}: only https URLs are allowed ({url})")
    log = log or (lambda m: print(f"fetch-deps: {m}", file=sys.stderr))
    rng = rng or random.Random()
    final = os.path.join(dest, file_name)
    if offline:  # verification only: works on a read-only directory, takes no lock, writes nothing
        if not os.path.exists(final):
            raise FetchError(f"{final}: not in the cache and --offline was given")
        actual = sha256_file(final)
        if actual != sha:
            raise FetchError(f"{final}: archive has SHA-256 {actual}, expected {sha}; not used")
        log(f"{file_name}: verified (offline)")
        return final
    os.makedirs(dest, exist_ok=True)
    lock_fd = os.open(os.path.join(dest, ".fetch-deps.lock"), os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        if os.path.exists(final):
            actual = sha256_file(final)
            if actual != sha:
                raise FetchError(f"{final}: cached archive has SHA-256 {actual}, expected {sha}; it is not "
                                 "used or replaced automatically (remove it after investigating)")
            log(f"{file_name}: verified cached archive")
            return final
        opener = urllib.request.build_opener(_HttpsOnlyRedirect())
        overall = time.monotonic() + deadline
        forbidden = 0
        last = None
        for attempt in range(1, attempts + 1):
            part = os.path.join(dest, f".{file_name}.part-{os.getpid()}-{rng.getrandbits(32):08x}")
            fd = os.open(part, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.close(fd)
            try:
                _download_once(url, part, sha, timeout, overall, max_bytes, opener)
                os.chmod(part, 0o644)
                os.replace(part, final)  # atomic publication of a verified archive
                _fsync_dir(dest)
                log(f"{file_name}: downloaded and verified (attempt {attempt})")
                return final
            except _Retryable as e:
                last = e
                if isinstance(e, _Retryable403):
                    forbidden += 1
                    if forbidden > retry_403:
                        raise FetchError(f"{file_name}: {e} from {url}; giving up after {forbidden} refusal(s). "
                                         "This may be an access restriction (or a transient server refusal); "
                                         "it is not worked around. Retry later or provide the archive with "
                                         "--deps-dir/YAP_DEPS_DIR.") from None
                if attempt == attempts:
                    break
                delay = min(backoff_max, backoff_base * (2 ** (attempt - 1)))
                delay = delay / 2 + rng.uniform(0, delay / 2)  # jitter
                if e.delay_hint is not None:
                    delay = min(backoff_max, max(delay, e.delay_hint))
                if time.monotonic() + delay > overall:
                    raise FetchError(f"{file_name}: {e}; overall deadline of {deadline}s reached") from None
                log(f"{file_name}: attempt {attempt}/{attempts} failed: {e}; retrying in {delay:.1f}s")
                sleep(delay)
            except FetchError as e:
                raise FetchError(f"{file_name}: {e}") from None
            finally:
                try:
                    os.remove(part)
                except FileNotFoundError:
                    pass
        raise FetchError(f"{file_name}: {last}; giving up after {attempts} attempts")
    finally:
        os.close(lock_fd)


def selected(group, only):
    names = []
    for name, (file_name, _, _, groups) in DEPS.items():
        if only:
            if file_name in only or name in only:
                names.append(name)
        elif group == "all" or group in groups:
            names.append(name)
    return names


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest")
    ap.add_argument("--group", choices=("build", "test", "cometbft", "all"), default="all")
    ap.add_argument("--only", nargs="+", default=[])
    ap.add_argument("--offline", action="store_true", help="verify the cache only; never download")
    ap.add_argument("--attempts", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=30.0, help="connect/stall timeout per operation, seconds")
    ap.add_argument("--deadline", type=float, default=900.0, help="overall deadline per archive, seconds")
    ap.add_argument("--retry-403", type=int, default=1)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    if a.list:
        for name, (file_name, sha, url, groups) in DEPS.items():
            print(f"{file_name}  {sha}  {','.join(groups)}  {url}")
        return 0
    if not a.dest or a.attempts < 1 or a.retry_403 < 0 or a.timeout <= 0 or a.deadline <= 0:
        ap.print_usage(sys.stderr)
        return 1
    names = selected(a.group, set(a.only))
    if a.only and len(names) != len(set(a.only)):
        print(f"fetch-deps: unknown archive in --only {sorted(a.only)}", file=sys.stderr)
        return 1
    failed = False
    for name in names:
        try:
            fetch(name, a.dest, attempts=a.attempts, timeout=a.timeout, deadline=a.deadline,
                  retry_403=a.retry_403, offline=a.offline)
        except (FetchError, OSError) as e:
            print(f"fetch-deps: ERROR {e}", file=sys.stderr)
            failed = True
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
