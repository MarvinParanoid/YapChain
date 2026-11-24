#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Single-validator YapChain localnet with a real CometBFT v0.40.0 (M2.3).

This is NOT a BFT network: one validator, one CometBFT process, one yapd. It exists to run the real
ABCI++ protocol end to end. Everything (keys, wallet, databases) is created under one directory that
is never inside the repository; nothing here is meant to be committed.

    localnet.py build-cometbft --deps-dir DIR | --download   [--out build/cometbft]
    localnet.py init   --dir D --yap BIN --cometbft BIN [--chain-id ID]
    localnet.py start  --dir D --yapd BIN --cometbft BIN [--wait-height N]
    localnet.py stop   --dir D
    localnet.py status --dir D

Standard library only. Run with `python3 -I`."""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

# ADR-0027: CometBFT v0.40.0, built from the verified source archive of the tag.
COMETBFT_VERSION = "v0.40.0"
COMETBFT_COMMIT = "0880b4d378f347ab16e54ec677ff50d803f37d62"
COMETBFT_TARBALL = "cometbft-v0.40.0.tar.gz"
COMETBFT_SHA256 = "c458c51a452318a849905cdeb1eb1c4437cff0b5412bb6ca5d948c7c37d77a8a"
COMETBFT_URL = "https://github.com/cometbft/cometbft/archive/refs/tags/v0.40.0.tar.gz"

APP_VERSION = "1"  # yapd Info.app_version; genesis consensus_params.version.app must match (ADR-0028)
DEFAULT_CHAIN_ID = "yapchain-localnet-1"
GENESIS_ACCOUNTS = {"alice": "1000000YAP", "bob": "1000YAP"}


class LocalnetError(RuntimeError):
    pass


# ------------------------------------------------------------------ helpers

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise LocalnetError(f"{' '.join(map(str, cmd))} failed ({r.returncode}):\n{r.stdout}{r.stderr}")
    return r.stdout


def kv(text):
    """Parses `key: value` lines printed by `yap`."""
    out = {}
    for line in text.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            out[k] = v
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def load_meta(d):
    with open(os.path.join(d, "localnet.json")) as f:
        return json.load(f)


def write_private(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)


def rfc3339_ms(ms):
    t = datetime.datetime.fromtimestamp(ms // 1000, tz=datetime.timezone.utc)
    return t.strftime("%Y-%m-%dT%H:%M:%S") + f".{ms % 1000:03d}Z"


# ------------------------------------------------------------------ CometBFT build (ADR-0027)

def build_cometbft(deps_dir, download, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="cometbft-src-", dir=out_dir)
    try:
        archive = os.path.join(work, COMETBFT_TARBALL)
        if deps_dir:
            shutil.copyfile(os.path.join(deps_dir, COMETBFT_TARBALL), archive)
        elif download:
            urllib.request.urlretrieve(COMETBFT_URL, archive)
        else:
            raise LocalnetError("give --deps-dir with the verified archive or --download")
        got = sha256_file(archive)
        if got != COMETBFT_SHA256:
            raise LocalnetError(f"{COMETBFT_TARBALL}: SHA-256 {got} != pinned {COMETBFT_SHA256}")
        with tarfile.open(archive) as t:
            t.extractall(work, filter="data")
        src = [p for p in os.listdir(work) if p.startswith("cometbft-")]
        src = [p for p in src if os.path.isdir(os.path.join(work, p))]
        if len(src) != 1:
            raise LocalnetError("unexpected archive layout")
        src = os.path.join(work, src[0])
        binary = os.path.abspath(os.path.join(out_dir, "cometbft"))
        env = dict(os.environ, GOFLAGS="-mod=readonly", CGO_ENABLED="0")
        ldflags = f"-X github.com/cometbft/cometbft/version.TMGitCommitHash={COMETBFT_COMMIT[:8]}"
        run(["go", "mod", "verify"], cwd=src, env=env)
        run(["go", "build", "-trimpath", "-ldflags", ldflags, "-o", binary, "./cmd/cometbft"], cwd=src, env=env)
        go_version = run(["go", "version"]).strip()
        digest = sha256_file(binary)
        with open(binary + ".build-info", "w") as f:
            f.write(f"source {COMETBFT_TARBALL} sha256 {COMETBFT_SHA256}\ncommit {COMETBFT_COMMIT}\n"
                    f"toolchain {go_version}\nbinary sha256 {digest}\n")
        print(f"cometbft: {binary}\nsha256: {digest}\ntoolchain: {go_version}")
        return binary
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ------------------------------------------------------------------ init

def set_toml(text, section, key, value):
    """Replaces `key = ...` inside [section] (top level when section is None)."""
    lines = text.split("\n")
    current = None
    for i, line in enumerate(lines):
        m = re.match(r"^\[([^\]]+)\]\s*$", line)
        if m:
            current = m.group(1)
            continue
        if current == section and re.match(rf"^{re.escape(key)}\s*=", line):
            lines[i] = f"{key} = {value}"
            return "\n".join(lines)
    raise LocalnetError(f"config.toml: [{section}] {key} not found")


def init(d, yap, cometbft, chain_id=DEFAULT_CHAIN_ID, timeout_commit="1s"):
    d = os.path.abspath(d)
    if os.path.exists(os.path.join(d, "localnet.json")):
        raise LocalnetError(f"{d} already holds a localnet")
    os.makedirs(d, exist_ok=True)
    os.chmod(d, 0o700)
    cmt_home = os.path.join(d, "cometbft")
    wallet = os.path.join(d, "wallet")
    yapd_home = os.path.join(d, "yapd")
    os.makedirs(yapd_home, mode=0o700)
    socket_path = os.path.join(yapd_home, "yapd.sock")
    if len(socket_path) >= 100:
        raise LocalnetError(f"socket path too long for a Unix socket ({len(socket_path)}): use a shorter --dir")

    # 1. CometBFT home: validator key, node key, default config and genesis.
    run([cometbft, "init", "--home", cmt_home])
    with open(os.path.join(cmt_home, "config", "priv_validator_key.json")) as f:
        pv = json.load(f)
    if pv["pub_key"]["type"] != "tendermint/PubKeyEd25519":
        raise LocalnetError("validator key is not Ed25519")
    val_pubkey = base64.b64decode(pv["pub_key"]["value"]).hex()

    # 2. Wallet keys through the real CLI (random password, file mode 0600).
    password = os.path.join(d, "wallet.password")
    write_private(password, secrets.token_hex(24) + "\n")
    addrs = {}
    for name in list(GENESIS_ACCOUNTS) + ["validator-reward"]:
        out = kv(run([yap, "--home", wallet, "--password-file", password, "keys", "create", name]))
        addrs[name] = out["address"]

    # 3. YapChain genesis (protocol.md §11) through the CLI; time = now, whole milliseconds.
    genesis_ms = (int(time.time()) - 1) * 1000
    yap_genesis = os.path.join(d, "yap-genesis.json")
    cmd = [yap, "genesis", "new", "--chain-id", chain_id, "--time-ms", str(genesis_ms), "--out", yap_genesis,
           "--validator", f"{val_pubkey}:10:{addrs['validator-reward']}:validator-1"]
    for name, amount in GENESIS_ACCOUNTS.items():
        cmd += ["--account", f"{addrs[name]}={amount}"]
    run(cmd)
    genesis_hash = kv(run([yap, "genesis", "hash", yap_genesis])).get("genesis_hash", "")

    # 4. CometBFT genesis: our genesis embedded as app_state (ADR-0016), app_hash empty (§11.4).
    cmt_genesis_path = os.path.join(cmt_home, "config", "genesis.json")
    with open(cmt_genesis_path) as f:
        g = json.load(f)
    with open(yap_genesis) as f:
        app_state = json.load(f)
    g["chain_id"] = chain_id
    g["genesis_time"] = rfc3339_ms(genesis_ms)
    g["initial_height"] = "1"
    g["app_hash"] = ""
    g["app_state"] = app_state
    g["consensus_params"]["version"]["app"] = APP_VERSION
    g["consensus_params"]["abci"]["vote_extensions_enable_height"] = "0"
    if len(g["validators"]) != 1 or g["validators"][0]["power"] != "10":
        raise LocalnetError("unexpected default validator set")
    g["validators"][0]["name"] = "validator-1"
    with open(cmt_genesis_path, "w") as f:
        json.dump(g, f, indent=2)
        f.write("\n")

    # 5. config.toml: Unix socket to yapd, loopback only, no peer exchange, flood mempool.
    rpc_port, p2p_port = free_port(), free_port()
    cfg_path = os.path.join(cmt_home, "config", "config.toml")
    with open(cfg_path) as f:
        cfg = f.read()
    for section, key, value in [
        (None, "proxy_app", f'"unix://{socket_path}"'),
        (None, "abci", '"socket"'),
        (None, "moniker", '"yap-localnet"'),
        ("rpc", "laddr", f'"tcp://127.0.0.1:{rpc_port}"'),
        ("p2p", "laddr", f'"tcp://127.0.0.1:{p2p_port}"'),
        ("p2p", "pex", "false"),
        ("mempool", "type", '"flood"'),
        ("mempool", "recheck", "true"),
        ("consensus", "timeout_commit", f'"{timeout_commit}"'),
        ("consensus", "create_empty_blocks", "true"),
    ]:
        cfg = set_toml(cfg, section, key, value)
    with open(cfg_path, "w") as f:
        f.write(cfg)

    meta = {
        "chain_id": chain_id,
        "genesis_time_ms": genesis_ms,
        "genesis_hash": genesis_hash,
        "rpc": f"http://127.0.0.1:{rpc_port}",
        "p2p_port": p2p_port,
        "socket": socket_path,
        "cometbft_home": cmt_home,
        "yapd_home": yapd_home,
        "wallet_home": wallet,
        "password_file": password,
        "yap_genesis": yap_genesis,
        "addresses": addrs,
        "validator_pubkey": val_pubkey,
        "validator_address": pv["address"],
    }
    with open(os.path.join(d, "localnet.json"), "w") as f:
        json.dump(meta, f, indent=2)
    return meta


# ------------------------------------------------------------------ processes

def rpc(meta, method, timeout=5, **params):
    q = urllib.parse.urlencode(params)
    url = f"{meta['rpc']}/{method}" + (f"?{q}" if q else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = json.load(r)
    except urllib.error.HTTPError as e:  # CometBFT answers RPC errors with HTTP 500 and a JSON body
        try:
            body = json.load(e)
        except ValueError:
            raise LocalnetError(f"RPC {method}: HTTP {e.code}") from None
    if "error" in body:
        raise LocalnetError(f"RPC {method}: {body['error']}")
    return body["result"]


def height(meta):
    return int(rpc(meta, "status")["sync_info"]["latest_block_height"])


def _pid(d, name):
    p = os.path.join(d, f"{name}.pid")
    if not os.path.exists(p):
        return None
    pid = int(open(p).read().strip())
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    return pid


def _spawn(d, name, cmd, env=None):
    if _pid(d, name):
        raise LocalnetError(f"{name} is already running")
    log = open(os.path.join(d, f"{name}.log"), "a")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True, env=env)
    with open(os.path.join(d, f"{name}.pid"), "w") as f:
        f.write(f"{p.pid}\n")
    return p


def wait_for(cond, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if cond():
                return
        except (OSError, urllib.error.URLError, LocalnetError, ValueError, KeyError):
            pass
        time.sleep(0.2)
    raise LocalnetError(f"timeout waiting for {what}")


def start_yapd(d, yapd, extra_env=None, trace=True):
    meta = load_meta(d)
    env = dict(os.environ)
    env.update(extra_env or {})
    cmd = [yapd, "--home", meta["yapd_home"], "--socket", meta["socket"]]
    if trace:
        cmd.append("--trace-abci")
    p = _spawn(d, "yapd", cmd, env)
    wait_for(lambda: os.path.exists(meta["socket"]) or p.poll() is not None, 30, "yapd socket")
    if p.poll() is not None:
        raise LocalnetError(f"yapd exited with {p.returncode}, see {d}/yapd.log")
    return p


def start_cometbft(d, cometbft):
    meta = load_meta(d)
    return _spawn(d, "cometbft", [cometbft, "start", "--home", meta["cometbft_home"]])


def wait_height(d, h, timeout=60):
    meta = load_meta(d)
    wait_for(lambda: height(meta) >= h, timeout, f"block height {h}")
    return height(meta)


def stop_process(d, name, timeout=30):
    """SIGTERM, then wait; SIGKILL only if it does not stop. Returns the exit status if known."""
    pid = _pid(d, name)
    if pid is None:
        return None
    os.kill(pid, signal.SIGTERM)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                os.remove(os.path.join(d, f"{name}.pid"))
                return os.waitstatus_to_exitcode(status)
        except ChildProcessError:  # not our child (started by another invocation)
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                os.remove(os.path.join(d, f"{name}.pid"))
                return None
        time.sleep(0.1)
    os.kill(pid, signal.SIGKILL)
    raise LocalnetError(f"{name} did not stop within {timeout}s (killed)")


def stop(d):
    # CometBFT first: it must not see the application disappear while it is running.
    stop_process(d, "cometbft")
    stop_process(d, "yapd")


# ------------------------------------------------------------------ CLI

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build-cometbft")
    b.add_argument("--deps-dir")
    b.add_argument("--download", action="store_true")
    b.add_argument("--out", default="build/cometbft")
    i = sub.add_parser("init")
    i.add_argument("--dir", required=True)
    i.add_argument("--yap", required=True)
    i.add_argument("--cometbft", required=True)
    i.add_argument("--chain-id", default=DEFAULT_CHAIN_ID)
    s = sub.add_parser("start")
    s.add_argument("--dir", required=True)
    s.add_argument("--yapd", required=True)
    s.add_argument("--cometbft", required=True)
    s.add_argument("--wait-height", type=int, default=2)
    t = sub.add_parser("stop")
    t.add_argument("--dir", required=True)
    st = sub.add_parser("status")
    st.add_argument("--dir", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build-cometbft":
            build_cometbft(a.deps_dir, a.download, a.out)
        elif a.cmd == "init":
            meta = init(a.dir, a.yap, a.cometbft, a.chain_id)
            print(json.dumps({k: meta[k] for k in ("chain_id", "rpc", "socket", "addresses")}, indent=2))
        elif a.cmd == "start":
            start_yapd(a.dir, a.yapd)
            start_cometbft(a.dir, a.cometbft)
            print(f"height {wait_height(a.dir, a.wait_height)}")
        elif a.cmd == "stop":
            stop(a.dir)
        elif a.cmd == "status":
            meta = load_meta(a.dir)
            info = rpc(meta, "abci_info")["response"]
            print(json.dumps({"height": height(meta), "abci_info": info,
                              "yapd_pid": _pid(a.dir, "yapd"), "cometbft_pid": _pid(a.dir, "cometbft")}, indent=2))
    except LocalnetError as e:
        print(f"localnet: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
