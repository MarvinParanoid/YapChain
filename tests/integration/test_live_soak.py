#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Distributed soak test (M2.5g): four validators and one non-validator full node under a long load of
transfers and burns from many senders, with nonce conflicts, resends, random stops/crashes/restarts,
partitions and quorum losses, all from a fixed seed. Registered for `ctest -C soak` only (too long for
the regular suite).

    test_live_soak.py <yap> <yapd> <cometbft> [--seed N] [--blocks 1000] [--txs 10000]

Senders' keys are derived from the seed (test keys, never written anywhere); transactions are built and
signed by the independent Python reference (tools/refgen/tx_ref.py) and must be accepted by yapd.
Everything is checked against the independent model of livenet.py at every common height.

Artifacts (always written to <base>/report/, no private keys): seed, genesis files, every transaction
sent (hex, hash, sender, nonce, node, time, CheckTx answer), the fault schedule and the executed events,
final heights and hashes, timing metrics, and copies of the node logs. The network directory (with the
nodes' keys) is deleted after success unless YAP_LIVE_KEEP=1.
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import signal
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from livenet import *  # noqa: E402,F401,F403 - the shared live-test library
from livenet import ln, bech32m_ref  # noqa: E402
import test_live_faults as tf  # noqa: E402 - fault helpers (frozen, partitions, blocksync evidence)
import tx_ref  # noqa: E402
import ed25519_ref  # noqa: E402

SENDERS = 16
SENDER_BALANCE = 100_000 * YIP


class Sender:
    def __init__(self, seed, k):
        self.seed = hashlib.sha256(b"yapchain-soak-sender/" + str(seed).encode() + b"/" + bytes([k])).digest()
        self.pub = ed25519_ref.public_key(self.seed)
        self.id = tx_ref.account_id(self.pub)
        self.addr = bech32m_ref.encode_address("tyap", self.id)
        self.name = f"s{k}"
        self.next = 0          # next nonce to use
        self.inflight = []     # [(nonce, hash, sent_at)]
        self.home = 0


def build(net, s, nonce, kind, amount, to=None, expires=0):
    body = {"version": 1, "chain_id": net.info["chain_id"], "sender_pubkey": s.pub, "nonce": nonce, "fee": FEE,
            "expires_at_height": expires, "memo": b"",
            "payload": {"type": "transfer", "to": to.id, "amount": amount} if kind == "transfer"
            else {"type": "burn", "amount": amount}}
    raw = tx_ref.sign_tx(body, s.seed)
    return raw.hex(), hashlib.sha256(raw).hexdigest().upper()


def make_schedule(rng, duration_s):
    """Fault events at increasing times; every kind appears at least once (partitions and quorum loss
    included), then random ones. Validators are 0..3, the full node is 4."""
    kinds = ["partition31", "quorum_loss", "partition22", "crash_validator", "restart_fullnode", "delay",
             "stop_validator", "crash_two"]
    t = 45.0
    out = []
    k = 0
    while t < duration_s:
        kind = kinds[k] if k < len(kinds) else rng.choice(kinds)
        out.append({"at_s": round(t, 1), "kind": kind, "node": rng.randrange(4), "down_s": rng.randint(8, 20),
                    "other": rng.randrange(4)})
        t += rng.uniform(40, 80)
        k += 1
    return out


class Soak:
    def __init__(self, net, model, seed, report):
        self.net, self.model, self.rng, self.report = net, model, random.Random(seed), report
        self.senders = None
        self.txlog = open(os.path.join(report, "transactions.jsonl"), "w")
        self.sent = 0
        self.inclusion = []      # seconds from first send to inclusion (sampled)
        self.pending_restore = []

    def up_nodes(self):
        return [n for n in self.net.nodes if n.alive("cometbft") and n.alive("yapd")]

    def send(self, node, tx_hex, h, s, nonce, kind):
        try:
            r = node.broadcast(tx_hex)
            code = int(r["code"])
        except ln.LocalnetError as e:
            code = "cache" if "tx already exists in cache" in str(e) else f"error: {str(e)[:80]}"
        except OSError as e:
            code = f"unreachable: {e}"
        self.txlog.write(json.dumps({"t": round(time.time(), 3), "hash": h, "sender": s.addr, "nonce": nonce,
                                     "kind": kind, "node": node.i, "answer": code, "hex": tx_hex}) + "\n")
        self.sent += 1
        ok = code in (0, "cache", 10) or str(code).startswith(("unreachable", "error"))
        check(ok, f"unexpected CheckTx answer {code} for {h[:16]}… via {node}")
        return code

    def committed_nonce(self, s, node):
        return int(node.query("/account/" + s.addr)[1]["nonce"])

    def step_load(self, depth):
        up = self.up_nodes()
        if not up:
            return
        q = self.rng.choice(up)
        try:
            h = q.height()
        except (OSError, ln.LocalnetError):
            return
        for s in self.senders:
            try:
                cn = self.committed_nonce(s, q)
            except (OSError, ln.LocalnetError, Fail):
                continue
            s.inflight = [x for x in s.inflight if x[0] >= cn]
            if not s.inflight or time.monotonic() - s.inflight[0][2] > 25:
                s.next = cn          # resync: lost (restarted mempool) or stuck transactions are rebuilt
                s.inflight = []
            home = next((n for n in up if n.i == s.home), None) or self.rng.choice(up)
            while len(s.inflight) < depth:
                kind = "burn" if self.rng.random() < 0.1 else "transfer"
                to = self.rng.choice([x for x in self.senders if x is not s])
                amount = self.rng.randint(1, 5_000)
                tx_hex, th = build(self.net, s, s.next, kind, amount, to, expires=h + 300)
                self.model.register_raw(tx_hex, kind, s.addr, amount, FEE, to.addr if kind == "transfer" else None)
                code = self.send(home, tx_hex, th, s, s.next, kind)
                if code != 0 and code != "cache":
                    s.inflight = []      # resync next round
                    break
                s.inflight.append((s.next, th, time.monotonic()))
                # controlled extras: resend of the same bytes elsewhere, conflicting same-nonce transfer
                r = self.rng.random()
                if r < 0.03 and len(up) > 1:
                    self.send(self.rng.choice([n for n in up if n is not home]), tx_hex, th, s, s.next, kind)
                elif r < 0.05 and len(up) > 1:
                    c_hex, ch = build(self.net, s, s.next, "transfer", amount + 1, to, expires=h + 300)
                    self.model.register_raw(c_hex, "transfer", s.addr, amount + 1, FEE, to.addr)
                    self.send(self.rng.choice([n for n in up if n is not home]), c_hex, ch, s, s.next, "conflict")
                s.next += 1
            if self.rng.random() < 0.05:
                s.home = self.rng.randrange(len(self.net.nodes))

    # ---- faults
    def apply(self, ev):
        net = self.net
        v = net.validators
        kind, i, j, down = ev["kind"], ev["node"], ev["other"], ev["down_s"]
        net.event(f"soak fault {kind} {ev}")
        now = time.monotonic()   # never jumps (wall-clock changes, host suspend)
        if kind == "partition31":
            net.fault("partition", groups=[[x for x in range(5) if x != i], [i]])
            self.pending_restore.append((now + down, ("heal",)))
        elif kind == "partition22":
            a = [i, (i + 1) % 4]
            net.fault("partition", groups=[a + [4], [x for x in range(4) if x not in a]])
            self.pending_restore.append((now + down, ("heal",)))
        elif kind == "quorum_loss":
            j = j if j != i else (i + 1) % 4
            for x in (i, j):
                if v[x].running():
                    v[x].stop()
            self.pending_restore.append((now + down, ("start", i)))
            self.pending_restore.append((now + down + 2, ("start", j)))
        elif kind == "crash_two":
            j = j if j != i else (i + 1) % 4
            for x in (i, j):
                if v[x].running():
                    v[x].kill("cometbft")
                    v[x].kill("yapd")
            self.pending_restore.append((now + down, ("start", i)))
            self.pending_restore.append((now + down, ("start", j)))
        elif kind == "crash_validator":
            if v[i].running():
                v[i].kill("cometbft")
                v[i].kill("yapd")
            self.pending_restore.append((now + down, ("start", i)))
        elif kind == "stop_validator":
            if v[i].running():
                v[i].stop()
            self.pending_restore.append((now + down, ("start", i)))
        elif kind == "restart_fullnode":
            fn = net.nodes[4]
            if fn.running():
                fn.stop()
            self.pending_restore.append((now + down, ("start", 4)))
        elif kind == "delay":
            net.fault("delay", ms=50, jitter_ms=25)
            self.pending_restore.append((now + down, ("nodelay",)))

    def restore_due(self, force=False):
        keep = []
        for at, act in self.pending_restore:
            if force or time.monotonic() >= at:
                if act[0] == "heal":
                    self.net.fault("heal")
                elif act[0] == "nodelay":
                    self.net.fault("delay", ms=0, jitter_ms=0)
                elif act[0] == "start":
                    n = self.net.nodes[act[1]]
                    if not n.running():
                        self.net.event(f"soak restart {n}")
                        n.start()
            else:
                keep.append((at, act))
        self.pending_restore = keep


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("yap")
    ap.add_argument("yapd")
    ap.add_argument("cometbft")
    ap.add_argument("--seed", type=int, default=int(os.environ.get("YAP_SOAK_SEED", "20261124")))
    ap.add_argument("--blocks", type=int, default=int(os.environ.get("YAP_SOAK_BLOCKS", "1000")))
    ap.add_argument("--txs", type=int, default=int(os.environ.get("YAP_SOAK_TXS", "10000")))
    a = ap.parse_args(argv)
    yap, yapd, cometbft = map(os.path.abspath, (a.yap, a.yapd, a.cometbft))
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="ys-", dir=os.environ.get("YAP_LIVE_PARENT"))
    d = os.path.join(base, "net")
    report = os.path.join(base, "report")
    os.makedirs(report, exist_ok=True)
    rng = random.Random(a.seed)
    senders = [Sender(a.seed, k) for k in range(SENDERS)]
    net = None
    ok = False
    t_start = time.monotonic()
    try:
        step("S0", f"seed {a.seed}: {SENDERS} senders, target {a.blocks} blocks and {a.txs} executed transactions")
        net = Net(d, yap, yapd, cometbft, fault_proxy=True, timeout_commit="300ms", proxy_seed=a.seed,
                  extra_accounts={s.addr: SENDER_BALANCE for s in senders})
        model = Model(net)
        soak = Soak(net, model, a.seed, report)
        soak.senders = senders
        for s in senders:
            s.home = rng.randrange(4)
        with open(os.path.join(report, "seed.json"), "w") as f:
            json.dump({"seed": a.seed, "senders": [s.addr for s in senders], "blocks": a.blocks, "txs": a.txs}, f,
                      indent=2)
        shutil.copy(net.info["yap_genesis"], os.path.join(report, "yap-genesis.json"))
        shutil.copy(os.path.join(net.nodes[0].meta["cometbft_home"], "config", "genesis.json"),
                    os.path.join(report, "cometbft-genesis.json"))
        for n in net.validators:
            n.start()
        net.wait_height(3, timeout=120)
        fn = net.add_full_node()
        fn.start()
        # The schedule covers the expected duration; events past the end are simply not reached.
        schedule = make_schedule(rng, duration_s=4 * 3600)
        with open(os.path.join(report, "fault-schedule.json"), "w") as f:
            json.dump(schedule, f, indent=2)

        step("S1", "load + faults until both targets are reached")
        t0 = time.monotonic()
        nxt = 0
        last_print = 0
        last_check = 0
        while True:
            el = time.monotonic() - t0
            if nxt < len(schedule) and el >= schedule[nxt]["at_s"] and not soak.pending_restore:
                soak.apply(schedule[nxt])
                nxt += 1
            soak.restore_due()
            soak.step_load(depth=3)
            up = [n for n in net.validators if n.alive("cometbft")]
            try:
                tip = max(n.height() for n in up) if up else 0
            except (OSError, ln.LocalnetError):
                tip = 0
            if tip - last_check >= 50 and up:
                try:
                    model.extend(tip - 1, up[0])
                except (OSError, ln.LocalnetError):
                    pass
                last_check = tip
            done_txs = len(model.included)
            if time.monotonic() - last_print > 30:
                note(f"t={el:.0f}s height {tip}, executed {done_txs}, sent {soak.sent}, faults applied {nxt}")
                last_print = time.monotonic()
            if tip >= a.blocks and done_txs >= a.txs and nxt >= 8:
                break
            check(el < 4 * 3600, "soak did not reach its targets within 4 hours")
            time.sleep(0.2)
        soak.restore_due(force=True)
        net.fault("heal")
        net.fault("delay", ms=0, jitter_ms=0)

        step("S2", "final synchronization of all five nodes")
        for n in net.nodes:
            if not n.running():
                n.start()
        wait_until(lambda: len({n.height() for n in net.nodes}) == 1 or
                   min(n.height() for n in net.nodes) >= max(n.height() for n in net.validators) - 1, 600,
                   "all five nodes at the tip")
        tip = net.wait_height(net.min_height(net.nodes) + 2, net.nodes, timeout=300)
        wait_until(net.mempools_empty, 300, "mempools to drain")
        tip = net.wait_height(net.min_height(net.nodes) + 2, net.nodes, timeout=300)

        step("S3", "every common height identical on all five nodes; state exact against the model")
        check_same_chain(net, net.nodes, tip - 1)
        tf.check_no_conflicts(net, net.nodes)
        for n in net.nodes:
            check_node_state(net, model, n, all_accounts(net) + [s.addr for s in senders])
        check_rewards(net, model, net.validators[0], 1, tip - 1)
        model.extend(tip - 1, net.nodes[0])
        executed = len(model.included)
        burns = sum(1 for h in model.included if model.txs[h]["kind"] == "burn")
        check(tip - 1 >= a.blocks, f"only {tip - 1} blocks")
        check(executed >= a.txs, f"only {executed} executed transactions")
        st = model.at(tip - 1)
        check(sum(st["bal"].values()) == st["minted"] - st["burned"], "model supply invariant")
        note(f"{tip - 1} blocks, {executed} transactions executed once each ({burns} burns), "
             f"{soak.sent} broadcasts; all five agree")

        step("S4", "metrics, final heights and hashes")
        n0 = net.validators[0]
        times = [tf.header_time_ms(n0.header(h)) for h in range(2, tip)]
        iv = [b - x for x, b in zip(times, times[1:])]
        flags = {}
        for v in net.validators:
            c = {ABSENT: 0, COMMIT: 0, NIL: 0}
            for h in range(2, tip):
                c[model.flags[h].get(v.address, ABSENT)] += 1
            flags[v.i] = c
        paid = {v.i: sum(model.rewards[h].get(v.reward, 0) for h in range(1, tip)) for v in net.validators}
        final = {n.i: {"height": n.height(), "block_hash_at_common": n.block(tip - 1)["block_id"]["hash"],
                       "root_at_common": n.roots()[tip - 1]} for n in net.nodes}
        metrics = {"seed": a.seed, "duration_s": round(time.monotonic() - t_start), "blocks": tip - 1,
                   "executed_txs": executed, "burns": burns, "broadcasts": soak.sent,
                   "interval_median_ms": round(tf.percentile(iv, 0.5)), "interval_p95_ms": round(tf.percentile(iv, 0.95)),
                   "interval_max_ms": round(max(iv)), "flags_per_validator": flags, "rewards_per_validator": paid,
                   "minted": st["minted"], "burned": st["burned"], "supply": st["minted"] - st["burned"],
                   "faults_applied": nxt, "final": final}
        with open(os.path.join(report, "metrics.json"), "w") as f:
            json.dump(metrics, f, indent=2)
        with open(os.path.join(report, "events.log"), "w") as f:
            f.write("\n".join(net.events) + "\n")
        shutil.copy(os.path.join(d, "faults.log"), os.path.join(report, "faults.log"))
        print("SOAK METRICS " + json.dumps(metrics), flush=True)

        step("S5", "graceful stop; verify-state of all five databases; logs kept without secrets")
        codes = net.stop_all()
        check(all(c.get("cometbft", 0) == 0 and c.get("yapd", 0) == 0 for c in codes.values()), f"exit codes {codes}")
        for n in net.nodes:
            vs = ln.kv(net.cli("verify-state", home=n.meta["yapd_home"]).stdout)
            last_h, last_root = n.commits()[-1]
            check(int(vs["height"]) == last_h and vs["state_root"] == last_root, f"{n}: verify-state {vs}")
            others = [m.roots().get(last_h) for m in net.nodes]
            check(all(r in (None, last_root) for r in others), f"{n}: root of {last_h} differs")
            note(f"{n}: verify-state OK at height {vs['height']}, root {vs['state_root']}")
        check_no_secrets(net)
        for n in net.nodes:
            for name in ("yapd", "cometbft"):
                shutil.copy(os.path.join(n.dir, f"{name}.log"), os.path.join(report, f"node{n.i}-{name}.log"))
        ok = True
        print(f"SOAK PASSED; report in {report}")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        if net is not None:
            print("\n".join(net.events[-30:]), file=sys.stderr)
        return 1
    finally:
        if net is not None:
            try:
                net.stop_all()
            except Exception as e:  # noqa: BLE001
                print(f"cleanup: {e}", file=sys.stderr)
        ln.kill_all(d)
        if ok and not os.environ.get("YAP_LIVE_KEEP"):
            shutil.rmtree(d, ignore_errors=True)   # keys and databases; the report stays
        print(f"artifacts in {base}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
