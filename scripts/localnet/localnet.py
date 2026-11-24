#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""YapChain localnet with a real CometBFT v0.40.0: one validator (M2.3) or N independent validators (M2.4).

One validator is NOT a BFT network; it runs the real ABCI++ protocol end to end. With --validators 4
every validator is a separate CometBFT process and a separate yapd with its own LMDB, ABCI socket,
ports, validator key, node key, logs and reward account; they talk only through CometBFT P2P on
loopback. Everything (keys, wallet, databases) is created under one directory that is never inside
the repository; nothing here is meant to be committed.

    localnet.py build-cometbft --deps-dir DIR | --download   [--out build/cometbft]
    localnet.py init   --dir D --yap BIN --cometbft BIN [--chain-id ID] [--validators N]
    localnet.py start  --dir D --yapd BIN --cometbft BIN [--node I] [--wait-height N]
    localnet.py stop   --dir D [--node I]
    localnet.py status --dir D

Layout: one validator — D itself is the node (D/localnet.json). N validators — D/network.json and
D/node<I>/ for each validator (each with its own localnet.json, cometbft/, yapd/, *.log, *.pid).

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
# The multi-validator network: more senders, and "dave" with a small balance for competing spends.
NETWORK_ACCOUNTS = {"alice": "1000000YAP", "bob": "1000YAP", "carol": "1000YAP", "dave": "10YAP"}
VALIDATOR_POWER = 10


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


def free_ports(n):
    ports = set()
    while len(ports) < n:
        ports.add(free_port())
    return sorted(ports)


def load_meta(d):
    """The node metadata (single-validator directory or one node of a network)."""
    with open(os.path.join(d, "localnet.json")) as f:
        return json.load(f)


def is_network(d):
    return os.path.exists(os.path.join(d, "network.json"))


def load_network(d):
    with open(os.path.join(d, "network.json")) as f:
        return json.load(f)


def node_dirs(d):
    """Node directories: [d] for one validator, d/node<I> for a network."""
    if not is_network(d):
        return [d]
    return [n["dir"] for n in load_network(d)["nodes"]]


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


def cometbft_init(cometbft, home):
    """`cometbft init`: validator key, node key, default config and genesis. Returns (pv, node_id)."""
    run([cometbft, "init", "--home", home])
    with open(os.path.join(home, "config", "priv_validator_key.json")) as f:
        pv = json.load(f)
    if pv["pub_key"]["type"] != "tendermint/PubKeyEd25519":
        raise LocalnetError("validator key is not Ed25519")
    node_id = run([cometbft, "show-node-id", "--home", home]).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", node_id):
        raise LocalnetError(f"unexpected node id {node_id!r}")
    return pv, node_id


def create_wallet(d, yap, names):
    """Wallet keys through the real CLI (random password, file mode 0600). Returns name -> address."""
    wallet = os.path.join(d, "wallet")
    password = os.path.join(d, "wallet.password")
    write_private(password, secrets.token_hex(24) + "\n")
    addrs = {}
    for name in names:
        out = kv(run([yap, "--home", wallet, "--password-file", password, "keys", "create", name]))
        addrs[name] = out["address"]
    return wallet, password, addrs


def patch_config(cfg_path, settings):
    with open(cfg_path) as f:
        cfg = f.read()
    for section, key, value in settings:
        cfg = set_toml(cfg, section, key, value)
    with open(cfg_path, "w") as f:
        f.write(cfg)


def common_settings(socket_path, moniker, rpc_port, p2p_port, timeout_commit):
    """config.toml for every node: Unix socket to its yapd, loopback only, no peer exchange, flood mempool
    with recheck, no state sync."""
    return [
        (None, "proxy_app", f'"unix://{socket_path}"'),
        (None, "abci", '"socket"'),
        (None, "moniker", f'"{moniker}"'),
        ("rpc", "laddr", f'"tcp://127.0.0.1:{rpc_port}"'),
        ("p2p", "laddr", f'"tcp://127.0.0.1:{p2p_port}"'),
        ("p2p", "pex", "false"),
        ("mempool", "type", '"flood"'),
        ("mempool", "recheck", "true"),
        ("statesync", "enable", "false"),
        ("consensus", "timeout_commit", f'"{timeout_commit}"'),
        ("consensus", "create_empty_blocks", "true"),
    ]


def embed_app_state(g, chain_id, genesis_ms, yap_genesis):
    """CometBFT genesis: our genesis embedded as app_state (ADR-0016), app_hash empty (§11.4)."""
    with open(yap_genesis) as f:
        app_state = json.load(f)
    g["chain_id"] = chain_id
    g["genesis_time"] = rfc3339_ms(genesis_ms)
    g["initial_height"] = "1"
    g["app_hash"] = ""
    g["app_state"] = app_state
    g["consensus_params"]["version"]["app"] = APP_VERSION
    g["consensus_params"]["abci"]["vote_extensions_enable_height"] = "0"
    return g


def init(d, yap, cometbft, chain_id=DEFAULT_CHAIN_ID, timeout_commit="1s", validators=1):
    if validators != 1:
        return init_network(d, yap, cometbft, validators, chain_id, timeout_commit)
    d = os.path.abspath(d)
    if os.path.exists(os.path.join(d, "localnet.json")) or is_network(d):
        raise LocalnetError(f"{d} already holds a localnet")
    os.makedirs(d, exist_ok=True)
    os.chmod(d, 0o700)
    cmt_home = os.path.join(d, "cometbft")
    yapd_home = os.path.join(d, "yapd")
    os.makedirs(yapd_home, mode=0o700)
    socket_path = os.path.join(yapd_home, "yapd.sock")
    if len(socket_path) >= 100:
        raise LocalnetError(f"socket path too long for a Unix socket ({len(socket_path)}): use a shorter --dir")

    # 1. CometBFT home: validator key, node key, default config and genesis.
    pv, _ = cometbft_init(cometbft, cmt_home)
    val_pubkey = base64.b64decode(pv["pub_key"]["value"]).hex()

    # 2. Wallet keys through the real CLI.
    wallet, password, addrs = create_wallet(d, yap, list(GENESIS_ACCOUNTS) + ["validator-reward"])

    # 3. YapChain genesis (protocol.md §11) through the CLI; time = now, whole milliseconds.
    genesis_ms = (int(time.time()) - 1) * 1000
    yap_genesis = os.path.join(d, "yap-genesis.json")
    cmd = [yap, "genesis", "new", "--chain-id", chain_id, "--time-ms", str(genesis_ms), "--out", yap_genesis,
           "--validator", f"{val_pubkey}:{VALIDATOR_POWER}:{addrs['validator-reward']}:validator-1"]
    for name, amount in GENESIS_ACCOUNTS.items():
        cmd += ["--account", f"{addrs[name]}={amount}"]
    run(cmd)
    genesis_hash = kv(run([yap, "genesis", "hash", yap_genesis])).get("genesis_hash", "")

    # 4. CometBFT genesis.
    cmt_genesis_path = os.path.join(cmt_home, "config", "genesis.json")
    with open(cmt_genesis_path) as f:
        g = embed_app_state(json.load(f), chain_id, genesis_ms, yap_genesis)
    if len(g["validators"]) != 1 or g["validators"][0]["power"] != str(VALIDATOR_POWER):
        raise LocalnetError("unexpected default validator set")
    g["validators"][0]["name"] = "validator-1"
    with open(cmt_genesis_path, "w") as f:
        json.dump(g, f, indent=2)
        f.write("\n")

    # 5. config.toml.
    rpc_port, p2p_port = free_ports(2)
    patch_config(os.path.join(cmt_home, "config", "config.toml"),
                 common_settings(socket_path, "yap-localnet", rpc_port, p2p_port, timeout_commit))

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


def init_network(d, yap, cometbft, n, chain_id=DEFAULT_CHAIN_ID, timeout_commit="1s"):
    """N independent validators of equal power sharing one byte-identical genesis (M2.4)."""
    if n < 2 or n > 16:
        raise LocalnetError("--validators must be 1 or 2..16")
    d = os.path.abspath(d)
    if os.path.exists(os.path.join(d, "localnet.json")) or is_network(d):
        raise LocalnetError(f"{d} already holds a localnet")
    os.makedirs(d, exist_ok=True)
    os.chmod(d, 0o700)

    # 1. Per node: its own CometBFT home (validator key, node key), its own yapd home and socket.
    nodes = []
    for i in range(n):
        nd = os.path.join(d, f"node{i}")
        os.makedirs(nd, mode=0o700)
        yapd_home = os.path.join(nd, "yapd")
        os.makedirs(yapd_home, mode=0o700)
        socket_path = os.path.join(yapd_home, "yapd.sock")
        if len(socket_path) >= 100:
            raise LocalnetError(f"socket path too long for a Unix socket ({len(socket_path)}): use a shorter --dir")
        cmt_home = os.path.join(nd, "cometbft")
        pv, node_id = cometbft_init(cometbft, cmt_home)
        nodes.append({"index": i, "dir": nd, "moniker": f"validator-{i}", "cometbft_home": cmt_home,
                      "yapd_home": yapd_home, "socket": socket_path, "node_id": node_id,
                      "validator_address": pv["address"], "pub_key_b64": pv["pub_key"]["value"],
                      "validator_pubkey": base64.b64decode(pv["pub_key"]["value"]).hex()})
    if len({x["validator_address"] for x in nodes}) != n or len({x["node_id"] for x in nodes}) != n:
        raise LocalnetError("validator keys or node keys are not distinct")

    # 2. Wallet: test accounts and one reward account per validator.
    rewards = [f"reward-{i}" for i in range(n)]
    wallet, password, addrs = create_wallet(d, yap, list(NETWORK_ACCOUNTS) + rewards)

    # 3. One YapChain genesis with all validators (power 10 each).
    genesis_ms = (int(time.time()) - 1) * 1000
    yap_genesis = os.path.join(d, "yap-genesis.json")
    cmd = [yap, "genesis", "new", "--chain-id", chain_id, "--time-ms", str(genesis_ms), "--out", yap_genesis]
    for x in nodes:
        cmd += ["--validator", f"{x['validator_pubkey']}:{VALIDATOR_POWER}:{addrs[rewards[x['index']]]}:{x['moniker']}"]
    for name, amount in NETWORK_ACCOUNTS.items():
        cmd += ["--account", f"{addrs[name]}={amount}"]
    run(cmd)
    genesis_hash = kv(run([yap, "genesis", "hash", yap_genesis])).get("genesis_hash", "")

    # 4. One CometBFT genesis, written byte for byte to every node.
    with open(os.path.join(nodes[0]["cometbft_home"], "config", "genesis.json")) as f:
        g = embed_app_state(json.load(f), chain_id, genesis_ms, yap_genesis)
    g["validators"] = [{"address": x["validator_address"],
                        "pub_key": {"type": "tendermint/PubKeyEd25519", "value": x["pub_key_b64"]},
                        "power": str(VALIDATOR_POWER), "name": x["moniker"]} for x in nodes]
    genesis_bytes = (json.dumps(g, indent=2) + "\n").encode()
    for x in nodes:
        with open(os.path.join(x["cometbft_home"], "config", "genesis.json"), "wb") as f:
            f.write(genesis_bytes)

    # 5. config.toml: loopback ports, every other validator as a persistent peer. Several nodes share
    #    127.0.0.1, so allow_duplicate_ip and a non-strict address book are required.
    ports = free_ports(2 * n)
    for x in nodes:
        x["rpc_port"], x["p2p_port"] = ports[2 * x["index"]], ports[2 * x["index"] + 1]
    for x in nodes:
        peers = ",".join(f"{y['node_id']}@127.0.0.1:{y['p2p_port']}" for y in nodes if y is not x)
        settings = common_settings(x["socket"], x["moniker"], x["rpc_port"], x["p2p_port"], timeout_commit) + [
            ("p2p", "persistent_peers", f'"{peers}"'),
            ("p2p", "allow_duplicate_ip", "true"),
            ("p2p", "addr_book_strict", "false"),
        ]
        patch_config(os.path.join(x["cometbft_home"], "config", "config.toml"), settings)

    # 6. Metadata: per node (same keys as a single-validator localnet) and for the network.
    for x in nodes:
        meta = {
            "chain_id": chain_id,
            "genesis_time_ms": genesis_ms,
            "genesis_hash": genesis_hash,
            "rpc": f"http://127.0.0.1:{x['rpc_port']}",
            "p2p_port": x["p2p_port"],
            "socket": x["socket"],
            "cometbft_home": x["cometbft_home"],
            "yapd_home": x["yapd_home"],
            "wallet_home": wallet,
            "password_file": password,
            "yap_genesis": yap_genesis,
            "addresses": addrs,
            "validator_pubkey": x["validator_pubkey"],
            "validator_address": x["validator_address"],
            "node_id": x["node_id"],
            "index": x["index"],
            "moniker": x["moniker"],
            "reward_address": addrs[rewards[x["index"]]],
        }
        with open(os.path.join(x["dir"], "localnet.json"), "w") as f:
            json.dump(meta, f, indent=2)
        x["rpc"] = meta["rpc"]
        x["reward_address"] = meta["reward_address"]
        del x["pub_key_b64"]
    net = {"chain_id": chain_id, "genesis_time_ms": genesis_ms, "genesis_hash": genesis_hash,
           "genesis_sha256": hashlib.sha256(genesis_bytes).hexdigest(), "validators": n,
           "wallet_home": wallet, "password_file": password, "yap_genesis": yap_genesis,
           "addresses": addrs, "nodes": nodes}
    with open(os.path.join(d, "network.json"), "w") as f:
        json.dump(net, f, indent=2)
    return net


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


def _die_with_parent():
    """preexec_fn: the child gets SIGKILL when the process that started it dies (prctl PR_SET_PDEATHSIG),
    so a killed test run cannot leave validators behind."""
    import ctypes
    if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL) != 0:
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG)")


def _spawn(d, name, cmd, env=None, die_with_parent=False):
    if _pid(d, name):
        raise LocalnetError(f"{name} is already running")
    log = open(os.path.join(d, f"{name}.log"), "a")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True, env=env,
                         preexec_fn=_die_with_parent if die_with_parent else None)
    log.close()
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


def start_yapd(d, yapd, extra_env=None, trace=True, die_with_parent=False):
    meta = load_meta(d)
    env = dict(os.environ)
    env.update(extra_env or {})
    cmd = [yapd, "--home", meta["yapd_home"], "--socket", meta["socket"]]
    if trace:
        cmd.append("--trace-abci")
    p = _spawn(d, "yapd", cmd, env, die_with_parent)
    wait_for(lambda: os.path.exists(meta["socket"]) or p.poll() is not None, 30, "yapd socket")
    if p.poll() is not None:
        raise LocalnetError(f"yapd exited with {p.returncode}, see {d}/yapd.log")
    return p


def start_cometbft(d, cometbft, die_with_parent=False):
    meta = load_meta(d)
    return _spawn(d, "cometbft", [cometbft, "start", "--home", meta["cometbft_home"]], None, die_with_parent)


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
    """Stops one node (a node directory) or every node of a network. CometBFT first: it must not see
    the application disappear while it is running."""
    nodes = node_dirs(d)
    for nd in nodes:
        stop_process(nd, "cometbft")
    for nd in nodes:
        stop_process(nd, "yapd")


def kill_all(d):
    """Best effort, never raises: SIGKILL every process recorded under d (cleanup after a failure)."""
    try:
        nodes = node_dirs(d)
    except (OSError, ValueError, KeyError):
        nodes = [d]
    for nd in nodes:
        for name in ("cometbft", "yapd"):
            try:
                pid = _pid(nd, name)
                if pid:
                    os.kill(pid, signal.SIGKILL)
            except (OSError, ValueError):
                pass


def node_dir(d, index):
    """The directory of node `index` (0-based) of a network, or d itself for one validator."""
    nodes = node_dirs(d)
    if index is None:
        return None
    if not is_network(d) and index != 0:
        raise LocalnetError("a single-validator localnet has only node 0")
    if index < 0 or index >= len(nodes):
        raise LocalnetError(f"no node {index} (0..{len(nodes) - 1})")
    return nodes[index]


def start_node(nd, yapd, cometbft, die_with_parent=False, yapd_env=None):
    """Starts yapd, then CometBFT, of one node with its existing data."""
    return start_yapd(nd, yapd, yapd_env, die_with_parent=die_with_parent), \
        start_cometbft(nd, cometbft, die_with_parent=die_with_parent)


def status(d):
    out = []
    for nd in node_dirs(d):
        meta = load_meta(nd)
        row = {"node": meta.get("moniker", "validator-1"), "rpc": meta["rpc"],
               "yapd_pid": _pid(nd, "yapd"), "cometbft_pid": _pid(nd, "cometbft")}
        try:
            st = rpc(meta, "status")
            row["height"] = int(st["sync_info"]["latest_block_height"])
            row["catching_up"] = st["sync_info"]["catching_up"]
            row["peers"] = int(rpc(meta, "net_info")["n_peers"])
            row["app_hash"] = st["sync_info"]["latest_app_hash"]
        except (OSError, LocalnetError, ValueError, KeyError) as e:
            row["error"] = str(e)
        out.append(row)
    return out


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
    i.add_argument("--validators", type=int, default=1, help="1 (default) or 2..16 independent validators")
    i.add_argument("--timeout-commit", default="1s")
    s = sub.add_parser("start")
    s.add_argument("--dir", required=True)
    s.add_argument("--yapd", required=True)
    s.add_argument("--cometbft", required=True)
    s.add_argument("--node", type=int, help="start only this node (0-based), with its existing data")
    s.add_argument("--wait-height", type=int, default=2)
    t = sub.add_parser("stop")
    t.add_argument("--dir", required=True)
    t.add_argument("--node", type=int, help="stop only this node; its data is kept")
    st = sub.add_parser("status")
    st.add_argument("--dir", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build-cometbft":
            build_cometbft(a.deps_dir, a.download, a.out)
        elif a.cmd == "init":
            meta = init(a.dir, a.yap, a.cometbft, a.chain_id, a.timeout_commit, a.validators)
            if a.validators == 1:
                print(json.dumps({k: meta[k] for k in ("chain_id", "rpc", "socket", "addresses")}, indent=2))
            else:
                print(json.dumps({"chain_id": meta["chain_id"], "genesis_sha256": meta["genesis_sha256"],
                                  "addresses": meta["addresses"],
                                  "nodes": [{k: x[k] for k in ("moniker", "rpc", "p2p_port", "node_id",
                                                               "validator_address")} for x in meta["nodes"]]},
                                 indent=2))
        elif a.cmd == "start":
            nodes = [node_dir(a.dir, a.node)] if a.node is not None else node_dirs(a.dir)
            for nd in nodes:
                start_yapd(nd, a.yapd)
            for nd in nodes:
                start_cometbft(nd, a.cometbft)
            for nd in nodes:
                print(f"{nd}: height {wait_height(nd, a.wait_height, timeout=120)}")
        elif a.cmd == "stop":
            if a.node is not None:
                nd = node_dir(a.dir, a.node)
                stop_process(nd, "cometbft")
                stop_process(nd, "yapd")
            else:
                stop(a.dir)
        elif a.cmd == "status":
            print(json.dumps(status(a.dir), indent=2))
    except (LocalnetError, OSError) as e:
        print(f"localnet: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
