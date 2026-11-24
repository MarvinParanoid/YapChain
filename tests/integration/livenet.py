# SPDX-License-Identifier: Apache-2.0
"""Shared library of the live multi-node tests (M2.4, M2.5): nodes, the network, an independent model
of the state built from consensus data, and checks that compare nodes strictly by height.

Values are compared strictly at the same height: block hashes and headers by height, every node's
state root of height H from its own Commit log, execution results by height. Account queries answer at
the node's latest committed height (historical queries are not supported); every answer is checked
against the model of the state AT THAT HEIGHT, built from consensus data (blocks and their last
commits) and the protocol rules (protocol.md §9.3, §9.5; economics.md §8).
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
        self.validator = info.get("validator", True)
        self.address = info["validator_address"].upper()
        self.reward = info.get("reward_address")
        self.procs = {}

    def __repr__(self):
        return f"validator-{self.i}" if self.validator else f"fullnode-{self.i}"

    # processes
    def start(self, yapd=None, yapd_env=None):
        self.start_yapd(yapd, yapd_env)
        self.start_cometbft()

    def start_yapd(self, yapd=None, env=None):
        self.procs["yapd"] = ln.start_yapd(self.dir, yapd or self.net.yapd, env, die_with_parent=True)

    def start_cometbft(self):
        self.procs["cometbft"] = ln.start_cometbft(self.dir, self.net.cometbft, die_with_parent=True)

    def running(self):
        return any(p.poll() is None for p in self.procs.values())

    def alive(self, name):
        p = self.procs.get(name)
        return p is not None and p.poll() is None

    def kill(self, name, sig=signal.SIGKILL):
        """Crash one component (SIGKILL by default); returns its exit status."""
        p = self.procs.pop(name)
        if p.poll() is None:
            p.send_signal(sig)
        try:
            rc = p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            p.kill()
            raise Fail(f"{self}: {name} did not exit after signal {sig}")
        pid_file = os.path.join(self.dir, f"{name}.pid")
        if os.path.exists(pid_file):
            os.remove(pid_file)
        self.net.event(f"{self}: {name} signal {int(sig)} -> exit {rc}")
        return rc

    def reap(self, name, timeout=60):
        """Waits for a component that exits by itself; returns its exit status."""
        p = self.procs.pop(name)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.procs[name] = p
            raise Fail(f"{self}: {name} did not exit within {timeout}s")
        pid_file = os.path.join(self.dir, f"{name}.pid")
        if os.path.exists(pid_file):
            os.remove(pid_file)
        return rc

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
    def __init__(self, d, yap, yapd, cometbft, fault_proxy=False, extra_accounts=None, timeout_commit="1s",
                 proxy_seed=None):
        self.d, self.yap, self.yapd, self.cometbft = d, yap, yapd, cometbft
        self.info = ln.init_network(d, yap, cometbft, N, timeout_commit=timeout_commit, fault_proxy=fault_proxy,
                                    extra_accounts=extra_accounts)
        self.extra_accounts = dict(extra_accounts or {})
        self.nodes = [Node(self, x) for x in self.info["nodes"]]
        self.validators = list(self.nodes)
        self.by_address = {n.address: n for n in self.nodes}
        self.addresses = self.info["addresses"]
        self.events = []
        self.proxy = ln.start_fault_proxy(d, die_with_parent=True, seed=proxy_seed) if fault_proxy else None

    def event(self, text):
        """Timestamped record of a fault or control action (printed and kept for the report)."""
        rec = f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {text}"
        self.events.append(rec)
        with open(os.path.join(self.d, "events.log"), "a") as f:
            f.write(rec + "\n")
        return rec

    def fault(self, cmd, **kw):
        r = ln.fault(self.d, {"cmd": cmd, **kw})
        self.event(f"fault {cmd} {json.dumps(kw) if kw else ''}".strip())
        return r

    def links(self):
        return ln.fault(self.d, {"cmd": "status"})["links"]

    def add_full_node(self):
        x = ln.add_full_node(self.d, self.cometbft)
        n = Node(self, x)
        self.nodes.append(n)
        self.event(f"added {n} (non-validator, empty databases)")
        return n

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
        self.event("graceful stop of all running nodes")
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
        self.reward_of = {n.address: n.reward for n in net.validators}
        self.txs = {}  # CometBFT hash (upper hex) -> dict(kind, sender, to, amount, fee)
        bal = {a[k]: v for k, v in GENESIS.items()}
        bal.update(net.extra_accounts)
        bal.update({n.reward: 0 for n in net.validators})
        bal.update({POOL: 0, TREASURY: 0})
        self.states = {0: {"bal": bal, "nonce": {}, "minted": sum(bal.values()), "burned": 0}}
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
        return self.register_raw(tx_hex, kind, self.net.addresses[sender], amount, fee,
                                 to and self.net.addresses[to])

    def register_raw(self, tx_hex, kind, sender_addr, amount, fee=FEE, to_addr=None):
        h = hashlib.sha256(bytes.fromhex(tx_hex)).hexdigest().upper()
        self.txs[h] = {"kind": kind, "sender": sender_addr, "to": to_addr, "amount": amount, "fee": fee}
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



ERR_EXPIRED, ERR_TTL_TOO_LONG, ERR_NONCE, ERR_INSUFFICIENT_FUNDS = 8, 9, 10, 11


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


def account_hex(addr):
    return bytes(bech32m_ref.decode_address(addr.encode(), "tyap")).hex()


def app_votes(node, since=0):
    """height -> {validator address: (power, flag)} as the application received decided_last_commit."""
    out = {}
    for m in re.finditer(r"ABCI FinalizeBlock height=(\d+) [^\n]*commit=\[([^\]]*)\]", node.log(since=since)):
        votes = {}
        for item in filter(None, m.group(2).split(",")):
            a, pw, fl = item.split(":")
            votes[a.upper()] = (int(pw), int(fl))
        out[int(m.group(1))] = votes
    return out


def check_rewards(net, model, node, lo, hi):
    """For every height lo..hi: the application received exactly CometBFT's last commit (addresses,
    powers, flags), and the emitted rewards (block events) equal the normative split over the
    COMMIT signers — to the last yip. Returns flag counts."""
    model.extend(hi, node)
    votes = app_votes(node)
    counts = {ABSENT: 0, COMMIT: 0, NIL: 0}
    for h in range(lo, hi + 1):
        flags = model.flags[h]
        if h > 1:
            check(h in votes, f"{node}: no FinalizeBlock trace for height {h}")
            got = {a: f for a, (pw, f) in votes[h].items()}
            check(got == flags, f"{node} height {h}: app decided_last_commit {got} != block LastCommit {flags}")
            check(all(pw == POWER for pw, _ in votes[h].values()), f"{node} height {h}: powers {votes[h]}")
        for f in flags.values():
            counts[f] += 1
        events = node.rpc("block_results", height=h)["finalize_block_events"] or []
        paid = {}
        block_reward = None
        for e in events:
            attrs = {a["key"]: a["value"] for a in e["attributes"]}
            if e["type"] == "validator_reward":
                paid[attrs["account"]] = int(attrs["amount"])
            elif e["type"] == "block_reward":
                block_reward = attrs
        want = {account_hex(addr): amount for addr, amount in model.rewards[h].items()}
        check(paid == want, f"height {h}: rewards paid {paid} != expected {want}")
        t = T_SHARE + V_SHARE - sum(want.values())
        check(block_reward is not None and int(block_reward["minted"]) == REWARD
              and int(block_reward["validators"]) == V_SHARE and int(block_reward["rewards_pool"]) == POOL_SHARE
              and int(block_reward["treasury"]) == t, f"height {h}: block_reward {block_reward}, treasury {t}")
    return counts


