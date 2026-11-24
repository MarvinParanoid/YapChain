#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live test: four independent validators — real CometBFT v0.40.0 + yapd + the real `yap` CLI (M2.4).

Every validator is a separate CometBFT process and a separate yapd with its own LMDB, socket, ports,
validator key and node key; they talk only through CometBFT P2P on loopback. Everything goes through
processes, RPC and logs; no library code is called.

Values are compared strictly at the same height: block hashes and headers by height, every node's
state root of height H from its own Commit log, execution results by height. Account queries answer at
the node's latest committed height (historical queries are not supported); every answer is checked
against an independent model of the state AT THAT HEIGHT, built here from consensus data (blocks and
their last commits) and the protocol rules (protocol.md §9.3, §9.5; economics.md §8).

    test_live_multival.py <yap> <yapd> <cometbft>

Environment: YAP_LIVE_DIR (base directory), YAP_LIVE_KEEP=1 (keep the directory after success).
On failure the directory, every node's logs and a tail of each log are kept/printed.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "localnet"))
sys.path.insert(0, os.path.join(ROOT, "tools", "refgen"))
import localnet as ln  # noqa: E402
import bech32m_ref  # noqa: E402

N = 4
POWER = 10
YIP = 1_000_000
GENESIS = {"alice": 1_000_000 * YIP, "bob": 1_000 * YIP, "carol": 1_000 * YIP, "dave": 10 * YIP}
REWARD, V_SHARE, POOL_SHARE, T_SHARE = 2_000_000, 1_200_000, 600_000, 200_000  # economics.md §8
FEE = 1_000
FEE_BURN_BPS = 5_000
ABSENT, COMMIT, NIL = 1, 2, 3


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def step(n, text):
    print(f"[{n}] {text}", flush=True)


def note(text):
    print(f"    {text}", flush=True)


def system_address(name):
    t = b"YAP/sys-account/v1"
    return bech32m_ref.encode_address("tyap", hashlib.sha256(bytes([len(t)]) + t + name).digest())


TREASURY = system_address(b"treasury")
POOL = system_address(b"rewards-pool")


def wait_until(cond, timeout, what, interval=0.25):
    """Polls an observable condition with a finite timeout (no fixed sleeps)."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            v = cond()
            if v:
                return v
        except (OSError, ln.LocalnetError, ValueError, KeyError) as e:
            last = e
        time.sleep(interval)
    raise Fail(f"timeout ({timeout}s) waiting for {what}" + (f": {last}" if last else ""))


# ------------------------------------------------------------------ nodes

class Node:
    def __init__(self, net, info):
        self.net = net
        self.i = info["index"]
        self.dir = info["dir"]
        self.meta = ln.load_meta(self.dir)
        self.address = info["validator_address"].upper()
        self.reward = info["reward_address"]
        self.procs = {}

    def __repr__(self):
        return f"validator-{self.i}"

    # processes
    def start(self, yapd=None):
        self.procs["yapd"], self.procs["cometbft"] = ln.start_node(
            self.dir, yapd or self.net.yapd, self.net.cometbft, die_with_parent=True)

    def running(self):
        return any(p.poll() is None for p in self.procs.values())

    def stop(self):
        """CometBFT first, then yapd (SIGTERM). Returns {name: exit status}."""
        codes = {}
        for name in ("cometbft", "yapd"):
            p = self.procs.pop(name, None)
            if p is None:
                continue
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
            try:
                codes[name] = p.wait(timeout=60)
            except subprocess.TimeoutExpired:
                p.kill()
                raise Fail(f"{self}: {name} did not stop within 60s")
            pid_file = os.path.join(self.dir, f"{name}.pid")
            if os.path.exists(pid_file):
                os.remove(pid_file)
        return codes

    # RPC
    def rpc(self, method, **params):
        return ln.rpc(self.meta, method, **params)

    def height(self):
        return ln.height(self.meta)

    def block(self, h):
        return self.rpc("block", height=h)

    def header(self, h):
        return self.block(h)["block"]["header"]

    def peers(self):
        return int(self.rpc("net_info")["n_peers"])

    def query(self, path):
        r = self.rpc("abci_query", path=f'"{path}"')["response"]
        check(int(r.get("code", 0)) == 0, f"{self}: query {path}: {r}")
        return int(r["height"]), json.loads(base64.b64decode(r["value"]))

    def broadcast(self, tx_hex):
        return self.rpc("broadcast_tx_sync", tx="0x" + tx_hex)

    # logs
    def log(self, name="yapd", since=0):
        with open(os.path.join(self.dir, f"{name}.log"), errors="replace") as f:
            return f.read()[since:]

    def log_size(self, name="yapd"):
        return os.path.getsize(os.path.join(self.dir, f"{name}.log"))

    def commits(self):
        """Every `Commit: height H, app_hash X` this node's yapd logged, in order (across restarts)."""
        return [(int(h), r) for h, r in re.findall(r"Commit: height (\d+), app_hash ([0-9a-f]{64})", self.log())]

    def roots(self):
        out = {}
        for h, r in self.commits():
            check(h not in out, f"{self}: height {h} committed twice")
            out[h] = r
        return out


class Net:
    def __init__(self, d, yap, yapd, cometbft):
        self.d, self.yap, self.yapd, self.cometbft = d, yap, yapd, cometbft
        self.info = ln.init(d, yap, cometbft, validators=N)
        self.nodes = [Node(self, x) for x in self.info["nodes"]]
        self.by_address = {n.address: n for n in self.nodes}
        self.addresses = self.info["addresses"]

    def up(self):
        return [n for n in self.nodes if n.running()]

    def min_height(self, nodes=None):
        return min(n.height() for n in (nodes or self.up()))

    def wait_height(self, h, nodes=None, timeout=120):
        nodes = nodes or self.up()
        wait_until(lambda: all(n.height() >= h for n in nodes), timeout, f"height {h} on {nodes}")
        return self.min_height(nodes)

    def cli(self, *args, home=None):
        cmd = [self.yap, "--home", home or self.info["wallet_home"], "--password-file", self.info["password_file"],
               *args]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail(f"{' '.join(cmd)} -> {r.returncode}\n{r.stdout}{r.stderr}")
        return r

    # ---- transactions (built and signed by the real CLI)
    def make_tx(self, kind, sender, nonce, amount, to=None, expires=None, fee=FEE, node=None):
        self.tx_counter = getattr(self, "tx_counter", 0) + 1
        out = os.path.join(self.d, f"tx-{self.tx_counter}.hex")
        expires = expires if expires is not None else (node or self.up()[0]).height() + 100
        args = ["tx", kind, "--from", sender, "--amount", str(amount), "--fee", str(fee), "--nonce", str(nonce),
                "--expires", str(expires), "--chain-id", self.info["chain_id"], "--out", out, "--yes"]
        if kind == "transfer":
            args += ["--to", self.addresses[to]]
        self.cli(*args)
        tx_hex = open(out).read().strip()
        return tx_hex, hashlib.sha256(bytes.fromhex(tx_hex)).hexdigest().upper()

    def find_tx(self, h, nodes=None):
        """{node index: (height, code)} for every node that has the transaction in a block."""
        out = {}
        for n in nodes or self.up():
            try:
                r = n.rpc("tx", hash="0x" + h)
                out[n.i] = (int(r["height"]), int(r["tx_result"].get("code", 0)))
            except ln.LocalnetError:
                pass
        return out

    def wait_included(self, h, timeout=60):
        """Waits until every running node has the transaction in a block; same height and code everywhere."""
        nodes = self.up()
        found = wait_until(lambda: (lambda f: f if len(f) == len(nodes) else None)(self.find_tx(h, nodes)),
                           timeout, f"transaction {h[:16]}… on every node")
        check(len(set(found.values())) == 1, f"transaction {h[:16]}…: nodes disagree {found}")
        height, code = next(iter(found.values()))
        check(code == 0, f"transaction {h[:16]}… included with code {code}")
        return height

    def mempools_empty(self):
        return all(int(n.rpc("num_unconfirmed_txs")["n_txs"]) == 0 for n in self.up())

    def stop_all(self):
        codes = {}
        for n in self.nodes:  # every CometBFT first, so no CometBFT sees its application vanish
            p = n.procs.get("cometbft")
            if p is not None and p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for n in self.nodes:
            if n.procs:
                codes[n.i] = n.stop()
        return codes


# ------------------------------------------------------------------ the model of the state

class Model:
    """Expected state after each height, from consensus data only (blocks, their last commits) and our
    own record of what every transaction we built does. Rules: protocol.md §9.3 (fees), §9.5 and
    economics.md §8 (emission). Every block transaction must be one we built and must have succeeded
    (ProcessProposal rejects blocks with failing transactions)."""

    def __init__(self, net):
        self.net = net
        a = net.addresses
        self.reward_of = {n.address: n.reward for n in net.nodes}
        self.txs = {}  # CometBFT hash (upper hex) -> dict(kind, sender, to, amount, fee)
        bal = {a[k]: v for k, v in GENESIS.items()}
        bal.update({n.reward: 0 for n in net.nodes})
        bal.update({POOL: 0, TREASURY: 0})
        self.states = {0: {"bal": bal, "nonce": {}, "minted": sum(GENESIS.values()), "burned": 0}}
        self.rewards = {}  # height -> {reward address: amount}
        self.signers = {}  # height -> [validator addresses with COMMIT in decided_last_commit]
        self.flags = {}    # height -> {validator address: flag}
        self.included = {}  # tx hash -> height
        self._valset = None

    def valset(self, node):
        """The validator set in CometBFT order (static: no validator updates exist)."""
        if self._valset is None:
            vs = node.rpc("validators", height=1, per_page=100)["validators"]
            check(len(vs) == N and all(int(v["voting_power"]) == POWER for v in vs), f"validator set {vs}")
            self._valset = [v["address"].upper() for v in vs]
        return self._valset

    def register(self, tx_hex, kind, sender, amount, fee=FEE, to=None):
        h = hashlib.sha256(bytes.fromhex(tx_hex)).hexdigest().upper()
        self.txs[h] = {"kind": kind, "sender": self.net.addresses[sender], "to": to and self.net.addresses[to],
                       "amount": amount, "fee": fee}
        return h

    def extend(self, upto, node):
        """Applies heights (last+1)..upto using node's blocks; the caller compares nodes separately."""
        last = max(self.states)
        for h in range(last + 1, upto + 1):
            blk = node.block(h)["block"]
            st = json.loads(json.dumps(self.states[h - 1]))
            bal, nonce = st["bal"], st["nonce"]
            # Transactions, in block order.
            for raw in blk["data"]["txs"] or []:
                th = hashlib.sha256(base64.b64decode(raw)).hexdigest().upper()
                check(th in self.txs, f"height {h}: unknown transaction {th}")
                check(th not in self.included, f"transaction {th} included twice ({self.included.get(th)} and {h})")
                self.included[th] = h
                t = self.txs[th]
                burn_part = t["fee"] * FEE_BURN_BPS // 10_000
                bal[t["sender"]] -= t["amount"] + t["fee"]
                check(bal[t["sender"]] >= 0, f"height {h}: negative balance of {t['sender']}")
                bal[TREASURY] += t["fee"] - burn_part
                st["burned"] += burn_part
                if t["kind"] == "transfer":
                    bal[t["to"]] = bal.get(t["to"], 0) + t["amount"]
                else:  # burn
                    st["burned"] += t["amount"]
                nonce[t["sender"]] = nonce.get(t["sender"], 0) + 1
            # Emission from decided_last_commit = this block's LastCommit (height H-1 signatures).
            # CometBFT orders the signatures like the validator set of H-1 and leaves the address of an
            # ABSENT signature empty (buildLastCommitInfo maps them by index, so do we).
            sigs = blk["last_commit"]["signatures"] if h > 1 else []
            check(h == 1 or len(sigs) == N, f"height {h}: {len(sigs)} signatures")
            vals = self.valset(node)
            flags = {}
            for i, s in enumerate(sigs):
                check(not s["validator_address"] or s["validator_address"].upper() == vals[i],
                      f"height {h}: signature {i} address {s['validator_address']} != validator {vals[i]}")
                flags[vals[i]] = int(s["block_id_flag"])
            self.flags[h] = flags
            signers = sorted(a for a, f in flags.items() if f == COMMIT)
            self.signers[h] = signers
            rewards = {}
            t = T_SHARE
            if signers:
                w = POWER * len(signers)
                for s in signers:
                    share = V_SHARE * POWER // w
                    rewards[self.reward_of[s]] = share
                    bal[self.reward_of[s]] += share
                t += V_SHARE - sum(rewards.values())
            else:
                t += V_SHARE  # height 1 / no signers: the validators' share goes to the treasury
            self.rewards[h] = rewards
            bal[POOL] += POOL_SHARE
            bal[TREASURY] += t
            st["minted"] += REWARD
            check(sum(bal.values()) == st["minted"] - st["burned"], f"model invariant broken at height {h}")
            self.states[h] = st
        return self.states[upto]

    def at(self, h):
        return self.states[h]


# ------------------------------------------------------------------ checks

def check_same_chain(net, nodes, hi, lo=1):
    """Block hash, header app_hash and execution results identical on every node at every height lo..hi,
    and Header(H+1).AppHash = each node's own state root of H (from its Commit log)."""
    roots = {n.i: n.roots() for n in nodes}
    for h in range(lo, hi + 1):
        ids = {n.i: n.block(h)["block_id"]["hash"] for n in nodes}
        check(len(set(ids.values())) == 1, f"height {h}: block hashes differ {ids}")
        apps = {n.i: n.header(h)["app_hash"] for n in nodes}
        check(len(set(apps.values())) == 1, f"height {h}: header app_hash differs {apps}")
        res = {n.i: json.dumps(n.rpc("block_results", height=h), sort_keys=True) for n in nodes}
        check(len(set(res.values())) == 1, f"height {h}: execution results differ between nodes")
        if h >= 2:
            for n in nodes:
                check(h - 1 in roots[n.i], f"{n}: no Commit of height {h - 1} in its log")
                check(apps[n.i].lower() == roots[n.i][h - 1],
                      f"{n}: header {h} app_hash {apps[n.i]} != its state root of {h - 1} {roots[n.i][h - 1]}")
    return roots


def check_node_state(net, model, node, names):
    """Every value a node answers is compared with the model at the height of that answer."""
    h, chain = node.query("/chain")
    model.extend(h, node)
    st = model.at(h)
    check(int(chain["total_minted"]) == st["minted"], f"{node} /chain minted at {h}: {chain['total_minted']} != {st['minted']}")
    check(int(chain["total_burned"]) == st["burned"], f"{node} /chain burned at {h}")
    check(int(chain["total_supply"]) == st["minted"] - st["burned"], f"{node} /chain supply at {h}")
    check(int(chain["total_supply"]) == int(chain["total_minted"]) - int(chain["total_burned"]),
          f"{node}: supply != minted - burned at {h}")
    check(int(chain["height"]) == h, f"{node}: /chain height field {chain['height']} != {h}")
    for addr in names:
        ha, acc = node.query("/account/" + addr)
        model.extend(ha, node)
        sa = model.at(ha)
        check(int(acc["balance"]) == sa["bal"].get(addr, 0),
              f"{node}: balance of {addr} at height {ha}: {acc['balance']} != {sa['bal'].get(addr, 0)}")
        check(int(acc["nonce"]) == sa["nonce"].get(addr, 0),
              f"{node}: nonce of {addr} at height {ha}: {acc['nonce']} != {sa['nonce'].get(addr, 0)}")
    return h


def all_accounts(net):
    return list(net.addresses.values()) + [POOL, TREASURY]


def check_no_secrets(net):
    """No log of any node contains the wallet password or a private key (validator key, node key), in
    base64 or in hex."""
    needles = [open(net.info["password_file"]).read().strip().lower()]
    for n in net.nodes:
        for f in ("priv_validator_key.json", "node_key.json"):
            with open(os.path.join(n.meta["cometbft_home"], "config", f)) as fh:
                b64 = json.load(fh)["priv_key"]["value"]
            needles += [b64.lower(), base64.b64decode(b64).hex()]
    check(all(len(x) >= 32 for x in needles), "unexpected secret format")
    checked = 0
    for n in net.nodes:
        for name in ("yapd", "cometbft"):
            text = n.log(name).lower()
            checked += len(text)
            for x in needles:
                check(x not in text, f"{n}/{name}.log contains a secret")
    return checked


def checktx_log(node, since=0):
    """[(type, tx hash, result code, last committed height)] of every CheckTx this node's yapd handled.
    Trace lines come in pairs under the application lock; a RECHECK follows the Commit it belongs to."""
    out = []
    committed = 0
    for m in re.finditer(r"Commit: height (\d+),|ABCI CheckTx type=(NEW|RECHECK) bytes=\d+ hash=([0-9a-f]{64})\n"
                         r"[^\n]*ABCI CheckTx result code=(\d+)", node.log(since=since)):
        if m.group(1):
            committed = int(m.group(1))
        else:
            out.append((m.group(2), m.group(3).upper(), int(m.group(4)), committed))
    return out


def broadcast_result(node, tx_hex):
    """CheckTx code from broadcast_tx_sync, or 'cache' when CometBFT's mempool cache refused the bytes."""
    try:
        return int(node.broadcast(tx_hex)["code"])
    except ln.LocalnetError as e:
        check("tx already exists in cache" in str(e), f"{node}: broadcast failed: {e}")
        return "cache"


def nonce_of(node, name, net):
    return int(node.query("/account/" + net.addresses[name])[1]["nonce"])


def process_proposals(node, since=0):
    """(height, proposer, result) of every ProcessProposal this node's yapd handled."""
    text = node.log(since=since)
    out = []
    for m in re.finditer(r"ABCI ProcessProposal height=(\d+) proposer=([0-9a-f]{40}) [^\n]*\n[^\n]*"
                         r"ABCI ProcessProposal result (ACCEPT|REJECT)", text):
        out.append((int(m.group(1)), m.group(2).upper(), m.group(3)))
    return out


# ------------------------------------------------------------------ scenario

def phase_consensus(net, model):
    step("B1", f"start {N} validators: separate CometBFT + yapd + LMDB + socket + ports + keys each")
    gen = {hashlib.sha256(open(os.path.join(n.meta["cometbft_home"], "config", "genesis.json"), "rb").read())
           .hexdigest() for n in net.nodes}
    check(gen == {net.info["genesis_sha256"]}, f"genesis.json differs between nodes: {gen}")
    check(len({n.meta["socket"] for n in net.nodes}) == N and len({n.meta["yapd_home"] for n in net.nodes}) == N,
          "sockets or databases are shared")
    for n in net.nodes:
        n.start()

    step("B2", "every node passes InitChain with the same genesis and the same app_hash")
    wait_until(lambda: all("InitChain: genesis" in n.log() for n in net.nodes), 60, "InitChain on every node")
    inits = set()
    for n in net.nodes:
        m = re.search(r"InitChain: genesis ([0-9a-f]{64}), app_hash ([0-9a-f]{64})", n.log())
        check(m.group(1) == net.info["genesis_hash"], f"{n}: genesis hash {m.group(1)}")
        req = re.search(r"ABCI InitChain chain_id=(\S+) initial_height=(\d+) validators=(\d+) app_version=(\d+) "
                        r"vote_ext_height=(\d+)", n.log())
        check(req and req.groups() == (net.info["chain_id"], "1", str(N), "1", "0"), f"{n}: InitChain {req}")
        inits.add(m.group(2))
    check(len(inits) == 1, f"InitChain app_hash differs: {inits}")
    root0 = inits.pop()
    note(f"genesis {net.info['genesis_hash'][:16]}…, InitChain app_hash {root0[:16]}… on all {N}")

    step("B3", "P2P: every node is connected to the other three")
    wait_until(lambda: all(n.peers() == N - 1 for n in net.nodes), 60, "full P2P mesh")

    step("B4", "at least 30 blocks")
    tip = net.wait_height(31, timeout=180)
    check(net.nodes[0].header(1)["app_hash"].lower() == root0, "header(1).app_hash != InitChain app_hash")

    step("B5", "every validator proposed at least once")
    proposers = {}
    for h in range(1, 31):
        proposers.setdefault(net.nodes[0].header(h)["proposer_address"], []).append(h)
    check(set(proposers) == set(net.by_address), f"proposers of 1..30: {sorted(proposers)}")
    note("proposed heights: " + "; ".join(f"{net.by_address[a]}: {len(hs)}" for a, hs in sorted(
        proposers.items(), key=lambda x: net.by_address[x[0]].i)))

    step("B6", "ProcessProposal runs on validators that are not the proposer, and accepts")
    for n in net.nodes:
        pp = process_proposals(n)
        foreign = [(h, p) for h, p, r in pp if p != n.address]
        check(foreign, f"{n}: no ProcessProposal of a foreign proposal")
        check(all(r == "ACCEPT" for _, _, r in pp), f"{n}: a proposal was rejected in a normal run")
        for h, p in foreign[:3]:
            check(net.nodes[0].header(h)["proposer_address"] == p, f"{n}: ProcessProposal proposer of {h}")
        note(f"{n}: {len(pp)} ProcessProposal, {len(foreign)} of them for other proposers")

    step("B7-B8", "same block hash, app_hash and results on every node; Header(H+1).AppHash = root(H)")
    check_same_chain(net, net.nodes, tip - 1)
    roots = {n.i: n.roots() for n in net.nodes}
    for h in range(1, tip):
        check(len({roots[i][h] for i in roots}) == 1, f"state root of {h} differs between nodes")
    note(f"heights 1..{tip - 1} identical on all {N} nodes; root({tip - 1}) = {roots[0][tip - 1][:16]}…")

    step("B9-B10", "identical execution, supply invariants on every node (model at each answer's height)")
    for n in net.nodes:
        h = check_node_state(net, model, n, all_accounts(net))
        note(f"{n}: state exact at height {h}")
    return tip



ERR_EXPIRED, ERR_TTL_TOO_LONG, ERR_NONCE, ERR_INSUFFICIENT_FUNDS = 8, 9, 10, 11


def phase_transactions(net, model):
    n0, n1, n2, n3 = net.nodes
    marks = {n.i: n.log_size() for n in net.nodes}
    YAP = YIP

    step("C1-C4", "Transfer via validator 0, Transfer via 1, Burn via 2, Transfer via 3 (different senders)")
    plan = [(n0, "transfer", "alice", "bob", 1_500_000), (n1, "transfer", "bob", "carol", 2 * YAP),
            (n2, "burn", "carol", None, 3 * YAP), (n3, "transfer", "dave", "alice", 1 * YAP)]
    sent = []
    for node, kind, sender, to, amount in plan:
        tx_hex, h = net.make_tx(kind, sender, nonce_of(node, sender, net), amount, to=to, node=node)
        model.register(tx_hex, kind, sender, amount, to=to)
        check(broadcast_result(node, tx_hex) == 0, f"{node} rejected {kind} of {sender}")
        sent.append((node, kind, sender, h))
    for node, kind, sender, h in sent:
        height = net.wait_included(h)
        seen = sorted(n.i for n in net.nodes if any(t == "NEW" and x == h for t, x, _, _ in checktx_log(n, marks[n.i])))
        check(node.i in seen and len(seen) >= 2, f"{kind} of {sender} via {node}: CheckTx only on {seen}")
        proposer = net.by_address[n0.header(height)["proposer_address"]]
        note(f"{kind} {sender} via {node}: height {height} (proposer {proposer}); "
             f"reached by P2P and checked on validators {seen}")

    step("C5", "five sequential nonces of one sender in one burst through validator 1")
    base = nonce_of(n1, "alice", net)
    burst = []
    for k in range(5):
        tx_hex, h = net.make_tx("transfer", "alice", base + k, 10_000 + k, to="bob", node=n1)
        model.register(tx_hex, "transfer", "alice", 10_000 + k, to="bob")
        burst.append((tx_hex, h))
    for tx_hex, _ in burst:
        check(broadcast_result(n1, tx_hex) == 0, "sequential nonce rejected by CheckTx")
    heights = [net.wait_included(h) for _, h in burst]
    check(heights == sorted(heights), f"nonces executed out of order: {heights}")
    note(f"nonces {base}..{base + 4} included at heights {heights}")

    step("C6", "the same bytes sent to three nodes are executed once; a later resend is refused")
    tx_hex, h = net.make_tx("transfer", "bob", nonce_of(n2, "bob", net), 500_000, to="alice", node=n2)
    model.register(tx_hex, "transfer", "bob", 500_000, to="alice")
    answers = {n.i: broadcast_result(n, tx_hex) for n in (n2, n0, n3)}
    check(answers[2] == 0 and all(a in (0, "cache") for a in answers.values()), f"same bytes: {answers}")
    height = net.wait_included(h)
    late = broadcast_result(n1, tx_hex)
    check(late in ("cache", ERR_NONCE), f"resend after inclusion: {late}")
    note(f"answers {answers}, included once at height {height}, resend to validator-1 -> {late}")

    step("C7", "conflicting transactions with the same nonce through validators 0 and 3")
    m = nonce_of(n0, "alice", net)
    a_hex, a = net.make_tx("transfer", "alice", m, 2 * YAP, to="bob", node=n0)
    b_hex, b = net.make_tx("transfer", "alice", m, 3 * YAP, to="carol", node=n3)
    model.register(a_hex, "transfer", "alice", 2 * YAP, to="bob")
    model.register(b_hex, "transfer", "alice", 3 * YAP, to="carol")
    ra, rb = parallel(lambda: broadcast_result(n0, a_hex), lambda: broadcast_result(n3, b_hex))
    check(ra in (0, ERR_NONCE) and rb in (0, ERR_NONCE) and 0 in (ra, rb), f"conflict CheckTx: {ra} {rb}")
    wait_until(lambda: nonce_of(n0, "alice", net) == m + 1 and nonce_of(n3, "alice", net) == m + 1, 60,
               "one of the conflicting transactions")
    net.wait_height(net.min_height() + 2)
    fa, fb = net.find_tx(a), net.find_tx(b)
    check((len(fa) == N) != (len(fb) == N) and not (fa and fb), f"conflict outcome A={fa} B={fb}")
    wait_until(net.mempools_empty, 30, "the losing transaction to leave every mempool")
    losers = {(b if fa else a): next(iter((fa or fb).values()))[0]}  # loser -> height of the winner
    note(f"CheckTx answers {ra}/{rb}; executed {'A (to bob)' if fa else 'B (to carol)'} only, nonce {m}->{m + 1}")

    step("C8-C9", "insufficient balance and TTL are refused by CheckTx on any node")
    poor_hex, _ = net.make_tx("transfer", "carol", nonce_of(n2, "carol", net), 10_000 * YAP, to="bob", node=n2)
    check(broadcast_result(n2, poor_hex) == ERR_INSUFFICIENT_FUNDS, "insufficient balance")
    tip = n3.height()
    old_hex, _ = net.make_tx("transfer", "alice", nonce_of(n3, "alice", net), 1, to="bob", expires=tip - 1, node=n3)
    check(broadcast_result(n3, old_hex) == ERR_EXPIRED, "expired transaction")
    far_hex, _ = net.make_tx("transfer", "alice", nonce_of(n3, "alice", net), 1, to="bob", expires=tip + 5_000, node=n3)
    check(broadcast_result(n3, far_hex) == ERR_TTL_TOO_LONG, "TTL above max_tx_ttl_blocks")

    step("C10", "competing spends of dave's whole balance through validators 1 and 2")
    k = nonce_of(n1, "dave", net)
    bal = int(n1.query("/account/" + net.addresses["dave"])[1]["balance"])
    d1_hex, d1 = net.make_tx("transfer", "dave", k, bal - FEE, to="bob", node=n1)
    d3_hex, d3 = net.make_tx("transfer", "dave", k, bal - FEE, to="carol", node=n2)
    d2_hex, _ = net.make_tx("transfer", "dave", k + 1, bal - FEE, to="alice", node=n1)
    model.register(d1_hex, "transfer", "dave", bal - FEE, to="bob")
    model.register(d3_hex, "transfer", "dave", bal - FEE, to="carol")
    r1, r3 = parallel(lambda: broadcast_result(n1, d1_hex), lambda: broadcast_result(n2, d3_hex))
    r2 = broadcast_result(n1, d2_hex)
    check(0 in (r1, r3) and r2 in (ERR_INSUFFICIENT_FUNDS, ERR_NONCE), f"competing spends: {r1} {r3} then {r2}")
    wait_until(lambda: nonce_of(n1, "dave", net) == k + 1, 60, "one competing spend")
    net.wait_height(net.min_height() + 2)
    f1, f3 = net.find_tx(d1), net.find_tx(d3)
    check((len(f1) == N) != (len(f3) == N) and not (f1 and f3), f"competing spends outcome {f1} {f3}")
    losers[d3 if f1 else d1] = next(iter((f1 or f3).values()))[0]
    for n in net.nodes:
        acc = n.query("/account/" + net.addresses["dave"])[1]
        check(acc["balance"] == "0", f"{n}: dave balance {acc['balance']} after spending everything once")
    wait_until(net.mempools_empty, 30, "empty mempools")
    note(f"answers {r1}/{r3}, second nonce {r2}; one spend executed, dave balance 0 on all nodes")

    step("C11", "non-empty mempool at Commit: real CheckTx RECHECK on live nodes")
    rechecks = {}
    burst_hashes = []
    for attempt in range(4):
        base = nonce_of(n0, "alice", net)
        for j in range(30):
            tx_hex, h = net.make_tx("transfer", "alice", base + j, 1_000 + j, to="carol", node=n0)
            model.register(tx_hex, "transfer", "alice", 1_000 + j, to="carol")
            check(broadcast_result(n0, tx_hex) == 0, f"burst tx {j} rejected")
            burst_hashes.append(h)
        for h in burst_hashes[-30:]:
            net.wait_included(h)
        rechecks = {n.i: [(x, c, ch) for t, x, c, ch in checktx_log(n, marks[n.i]) if t == "RECHECK"]
                    for n in net.nodes}
        if sum(map(len, rechecks.values())) > 0:
            break
    total = sum(map(len, rechecks.values()))
    check(total > 0, "no CheckTx RECHECK observed after 4 bursts of 30 transactions")
    for n in net.nodes:
        codes = {}
        for _, c, _ in rechecks[n.i]:
            codes[c] = codes.get(c, 0) + 1
        note(f"{n}: {len(rechecks[n.i])} RECHECK, result codes {codes}")
    # Every RECHECK in this phase concerns a transaction we built. A valid one stays valid. A conflict
    # loser is still valid after Commits below the winner's height and must fail with ERR_NONCE after
    # the Commit that executed the winner (then CometBFT drops it).
    evicted = set()
    for n in net.nodes:
        for x, c, committed in rechecks[n.i]:
            check(x in model.txs, f"{n}: RECHECK of an unknown transaction {x}")
            if x in losers:
                want = ERR_NONCE if committed >= losers[x] else 0
                check(c == want, f"{n}: RECHECK of a conflict loser after Commit {committed} (winner at "
                                 f"{losers[x]}) returned {c}, expected {want}")
                if c:
                    evicted.add(n.i)
            else:
                check(c == 0, f"{n}: a valid transaction failed RECHECK with {c} ({x[:16]}…)")
    note(f"RECHECK evicted losing conflicting transactions (ERR_NONCE) on validators {sorted(evicted)}"
         if evicted else "no conflict loser was left in a mempool at Commit this time (only valid RECHECKs)")

    step("C12", "no stuck transactions, no double execution, identical results everywhere")
    wait_until(net.mempools_empty, 30, "empty mempools after the burst")
    tip = net.wait_height(net.min_height() + 1)
    check_same_chain(net, net.nodes, tip - 1)
    for n in net.nodes:
        check_node_state(net, model, n, all_accounts(net))
    model.extend(tip - 1, n0)
    expected = [h for h in model.txs if h in model.included]
    for h in expected:
        found = net.find_tx(h)
        check(len(found) == N and len(set(found.values())) == 1, f"tx {h[:16]}…: {found}")
    note(f"{len(model.included)} transactions executed exactly once; all {N} nodes agree up to height {tip - 1}")
    return marks


def parallel(*fns):
    import threading
    out = [None] * len(fns)
    errs = []

    def run(i, f):
        try:
            out[i] = f()
        except BaseException as e:  # noqa: BLE001 - re-raised below
            errs.append(e)
    ts = [threading.Thread(target=run, args=(i, f)) for i, f in enumerate(fns)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if errs:
        raise errs[0]
    return out



def main(yap, yapd, cometbft):
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="yn-")
    d = os.path.join(base, "net")
    net = None
    ok = False
    try:
        net = Net(d, yap, yapd, cometbft)
        model = Model(net)
        phase_consensus(net, model)
        phase_transactions(net, model)

        step("Z", "graceful stop of all nodes; verify-state of all four databases")
        codes = net.stop_all()
        check(all(c == {"cometbft": 0, "yapd": 0} for c in codes.values()), f"exit codes {codes}")
        for n in net.nodes:
            vs = ln.kv(net.cli("verify-state", home=n.meta["yapd_home"]).stdout)
            last_h, last_root = n.commits()[-1]
            check(int(vs["height"]) == last_h and vs["state_root"] == last_root, f"{n}: verify-state {vs}")
            note(f"{n}: verify-state OK at height {vs['height']}, root {vs['state_root'][:16]}…")
        size = check_no_secrets(net)
        note(f"no password or private key in {size} bytes of node logs (traces enabled)")
        ok = True
        print("FOUR-VALIDATOR LIVE E2E PASSED")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        for i in range(N):
            for name in ("yapd", "cometbft"):
                p = os.path.join(d, f"node{i}", f"{name}.log")
                if os.path.exists(p):
                    print(f"--- tail node{i}/{name}.log", file=sys.stderr)
                    print("\n".join(open(p, errors="replace").read().splitlines()[-15:]), file=sys.stderr)
        return 1
    finally:
        if net is not None:
            try:
                net.stop_all()
            except Exception as e:  # noqa: BLE001 - cleanup must not hide the result
                print(f"cleanup: {e}", file=sys.stderr)
        ln.kill_all(d)
        if ok and not os.environ.get("YAP_LIVE_KEEP"):
            shutil.rmtree(base, ignore_errors=True)
        else:
            print(f"artifacts kept in {base}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*[os.path.abspath(p) for p in sys.argv[1:]]))
