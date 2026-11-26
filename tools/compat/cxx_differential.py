#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Differential test of two builds of `yap` (M2.6.2: the C++20 baseline against the C++23 build).

Everything runs through the real binaries as subprocesses, every command from a run directory with
the same relative layout (home/, txs/, genesis.json, pw), so the complete stdout of the two builds can
be compared byte for byte: CLI semantics and exit codes are part of what must not change.

  capture   --yap BASE --out DIR
      Runs a deterministic scenario with the baseline build and keeps everything in DIR: keys and the
      dev producer key (seed/), genesis files, every signed transaction and unsigned body (txs/), the
      planned commands, all outputs (record.json), the exported journal (journal.hex) and the chain
      database itself (home/). DIR is the preserved baseline; later modes only ever read it.

  compare   --dir DIR --yap CAND --work W
      Repeats the scenario with the candidate on identical inputs: genesis files, transaction bytes,
      tx ids, signing payloads, every produced block, every execution result (included / excluded
      with its code), the state root, supply, issuance, burned supply and every tracked balance and
      nonce after every block must be identical, and so must the exported journal. The candidate also
      replays the baseline journal from genesis and verifies the state.

  dbcompat  --dir DIR --base BASE --yap CAND --work W
      Opens copies of the baseline database with the candidate (verify-state, queries), continues the
      chain with both builds in lockstep on identical inputs, re-opens it (persistence), then lets the
      baseline open and extend the database the candidate wrote. The preserved DIR/home is never
      written: its files are hashed before and after.

Exit status 0 means no difference at all; any difference is printed and exits 1.
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "refgen"))
import bech32m_ref  # noqa: E402

PASSWORD = "differential-test-password"
ACCOUNTS = ["alice", "bob", "carol", "dave", "erin"]
VALIDATORS = [("val1", 10), ("val2", 20), ("val3", 30)]
GENESIS_FUNDS = {"alice": "1000YAP", "bob": "500YAP", "carol": "250.5YAP", "dave": "1YAP"}
CHAIN_ID = "yapchain-diff-1"
GENESIS_TIME = 1767225600000


class Diff(Exception):
    pass


def system_address(name):
    t = b"YAP/sys-account/v1"
    return bech32m_ref.encode_address("tyap", hashlib.sha256(bytes([len(t)]) + t + name).digest())


SYSTEM = [("treasury", system_address(b"treasury")), ("rewards-pool", system_address(b"rewards-pool"))]


class Run:
    """One build of yap working in one run directory (relative paths only)."""

    def __init__(self, yap, cwd):
        self.yap, self.cwd = os.path.abspath(yap), cwd

    def __call__(self, *args, expect=0):
        cmd = [self.yap, "--home", "home", "--password-file", "pw", *[str(a) for a in args]]
        env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": self.cwd}
        p = subprocess.run(cmd, cwd=self.cwd, capture_output=True, text=True, timeout=600, env=env)
        if expect is not None and p.returncode != expect:
            raise Diff(f"{' '.join(cmd[1:])}: exit {p.returncode}, expected {expect}\n{p.stdout}{p.stderr}")
        return {"rc": p.returncode, "out": p.stdout, "err": p.stderr}

    def kv(self, *args, expect=0):
        out = {}
        for line in self(*args, expect=expect)["out"].splitlines():
            if ": " in line:
                k, v = line.split(": ", 1)
                out.setdefault(k, v)
        return out


def prepare_run_dir(path, seed_home=None):
    os.makedirs(os.path.join(path, "txs"), exist_ok=True)
    with open(os.path.join(path, "pw"), "w") as f:
        f.write(PASSWORD + "\n")
    os.chmod(os.path.join(path, "pw"), 0o600)
    if seed_home:
        shutil.copytree(seed_home, os.path.join(path, "home"))


def sha_tree(path):
    h = {}
    for root, _, files in os.walk(path):
        for n in sorted(files):
            p = os.path.join(root, n)
            with open(p, "rb") as f:
                h[os.path.relpath(p, path)] = hashlib.sha256(f.read()).hexdigest()
    return h


def tamper(path):
    """Flips the last hex digit of a signed transaction file (breaks the signature)."""
    with open(path) as f:
        s = f.read().strip()
    s = s[:-1] + ("0" if s[-1] != "0" else "1")
    with open(path, "w") as f:
        f.write(s + "\n")


# ------------------------------------------------------------------ scenario

def genesis_variants(addr, pub):
    """(file, args) of the genesis files the builds must write identically."""
    vals = [f"--validator={pub[v]}:{p}:{addr[v]}:{v}" for v, p in VALIDATORS]
    accts = [f"--account={addr[a]}={amt}" for a, amt in GENESIS_FUNDS.items()]
    return [
        ("genesis.json", ["genesis", "new", "--chain-id", CHAIN_ID, "--time-ms", GENESIS_TIME, *accts, *vals]),
        ("genesis-single.json", ["genesis", "new", "--chain-id", "yapchain-diff-2", vals[0]]),
        ("genesis-many.json", ["genesis", "new", "--chain-id", "yapchain-diff-3", "--time-ms", "1800000000123",
                               *[f"--account={addr[a]}={i * 7919 + 1}" for i, a in enumerate(ACCOUNTS)], *vals]),
    ]


def tracked(addr):
    return [(n, addr[n]) for n in ACCOUNTS + [v for v, _ in VALIDATORS]] + SYSTEM


def snapshot(run, addr):
    """Everything observable about the committed state, as the raw CLI output."""
    snap = {"chain": run("query", "chain")["out"], "state_root": run("dev", "state-root")["out"]}
    for n, a in tracked(addr):
        snap["account:" + n] = run("query", "account", a, expect=None)  # absent accounts included
    return snap


def plan_block(rng, h, nonces, balances, addr, prefix):
    """Transactions for block h: (file, build commands, tamper) for a mix of valid and invalid ones."""
    txs, pending = [], dict(nonces)
    for i in range(rng.randint(0, 6)):
        name = f"{prefix}{h:03}-{i}.hex"
        sender = rng.choice([a for a in ACCOUNTS if balances.get(a, 0) > 10_000] or ["alice"])
        n = pending[sender]
        fee = rng.choice([1000, 1000, 1000, 1500, 4321])
        exp = h + rng.randint(1, 1800)
        kind = rng.choices(["transfer", "burn", "build-sign", "bad-nonce", "too-much", "expired",
                            "low-fee", "other-chain", "bad-sig", "duplicate"],
                           [30, 10, 10, 4, 4, 3, 3, 3, 3, 3])[0]
        to = rng.choice([a for a in ACCOUNTS if a != sender])
        amount = max(1, rng.randint(1, max(1, balances.get(sender, 0) // 50)))
        memo = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz0123456789 -_") for _ in range(rng.randint(0, 40)))
        common = ["--fee", fee, "--nonce", n, "--expires", exp]
        cmds, bad = [], False
        if kind == "transfer" or kind in ("bad-sig", "duplicate"):
            cmds.append(["tx", "transfer", "--from", sender, "--to", addr[to], "--amount", amount, "--memo", memo,
                         *common, "--out", f"txs/{name}"])
            bad = kind == "bad-sig"
        elif kind == "burn":
            cmds.append(["tx", "burn", "--from", sender, "--amount", f"{amount / 1e6:.6f}YAP", *common,
                         "--out", f"txs/{name}"])
        elif kind == "build-sign":
            body = f"txs/{name[:-4]}.body"
            cmds.append(["tx", "build", "--type", "transfer", "--from", sender, "--to", addr[to], "--amount", amount,
                         "--memo", memo, "--chain-id", CHAIN_ID, *common, "--out", body])
            cmds.append(["tx", "sign", "--key", sender, "--in", body, "--out", f"txs/{name}", "--yes"])
        elif kind == "bad-nonce":
            cmds.append(["tx", "transfer", "--from", sender, "--to", addr[to], "--amount", amount,
                         "--fee", fee, "--nonce", n + 3, "--expires", exp, "--out", f"txs/{name}"])
        elif kind == "too-much":
            cmds.append(["tx", "transfer", "--from", sender, "--to", addr[to], "--amount", "999999YAP",
                         *common, "--out", f"txs/{name}"])
        elif kind == "expired":
            cmds.append(["tx", "transfer", "--from", sender, "--to", addr[to], "--amount", amount,
                         "--fee", fee, "--nonce", n, "--expires", h - 1, "--out", f"txs/{name}"])
        elif kind == "low-fee":
            cmds.append(["tx", "transfer", "--from", sender, "--to", addr[to], "--amount", amount,
                         "--fee", 999, "--nonce", n, "--expires", exp, "--out", f"txs/{name}"])
        elif kind == "other-chain":
            cmds.append(["tx", "build", "--type", "burn", "--from", sender, "--amount", amount, "--chain-id",
                         "yapchain-other", *common, "--out", f"txs/{name[:-4]}.body"])
            cmds.append(["tx", "sign", "--key", sender, "--in", f"txs/{name[:-4]}.body", "--out", f"txs/{name}",
                         "--yes"])
        if kind in ("transfer", "burn", "build-sign"):
            pending[sender] = n + 1
        txs.append({"file": name, "kind": kind, "cmds": cmds, "tamper": bad})
        if kind == "duplicate":
            txs.append({"file": name, "kind": "duplicate-again", "cmds": [], "tamper": False})
            pending[sender] = n + 1
    return txs


def build_txs(run, txs):
    """Runs the build commands; returns every command's output and the produced file bytes (None for a
    file the build refused to write: such a refusal is itself an output both builds must agree on)."""
    outs, files = [], {}
    for t in txs:
        for c in t["cmds"]:
            outs.append(run(*c, expect=None))
        if t["cmds"] and t["tamper"] and os.path.exists(os.path.join(run.cwd, "txs", t["file"])):
            tamper(os.path.join(run.cwd, "txs", t["file"]))
        for c in t["cmds"]:
            path = os.path.join(run.cwd, c[c.index("--out") + 1])
            files[c[c.index("--out") + 1]] = open(path).read() if os.path.exists(path) else None
    return outs, files


def produce_cmd(t_ms, txs, files):
    """dev produce over every transaction file that exists (duplicates included twice)."""
    return ["dev", "produce", "--time-ms", t_ms] + [x for t in txs if files.get(f"txs/{t['file']}") is not None
                                                    for x in ("--tx", f"txs/{t['file']}")]


def account_state(run, addr):
    nonces, balances = {}, {}
    for a in ACCOUNTS:
        k = run.kv("query", "account", addr[a], expect=None)
        nonces[a] = int(k.get("nonce", "0"))
        balances[a] = int(k.get("balance_yip", "0"))
    return nonces, balances


def setup_keys(run):
    addr, pub = {}, {}
    for n in ACCOUNTS + [v for v, _ in VALIDATORS]:
        k = run.kv("keys", "create", n)
        addr[n] = k["address"]
        pub[n] = run.kv("keys", "show", n)["public_key"]
    return addr, pub


# ------------------------------------------------------------------ modes

def capture(a):
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        raise SystemExit(f"{out} exists; the baseline is never overwritten")
    prepare_run_dir(out)
    run = Run(a.yap, out)
    addr, pub = setup_keys(run)
    rec = {"addr": addr, "pub": pub, "genesis": {}, "blocks": [], "seed": a.seed, "n_blocks": a.blocks}
    for f, args in genesis_variants(addr, pub):
        run(*args, "--out", f)
        rec["genesis"][f] = {"args": args, "hash": run("genesis", "hash", f)["out"],
                             "validate": run("genesis", "validate", f, expect=None)}
    rec["init"] = run("dev", "init", "--genesis", "genesis.json")["out"]
    # The seed holds the keys and the dev producer key, never chain data: every later run starts there.
    os.makedirs(os.path.join(out, "seed"))
    for d in ("keys", "dev"):
        shutil.copytree(os.path.join(out, "home", d), os.path.join(out, "seed", d))
    rec["snap0"] = snapshot(run, addr)
    rng = random.Random(a.seed)
    t_ms = GENESIS_TIME
    for h in range(1, a.blocks + 1):
        nonces, balances = account_state(run, addr)
        txs = plan_block(rng, h, nonces, balances, addr, "b")
        outs, files = build_txs(run, txs)
        t_ms += rng.choice([1, 999, 2000, 2000, 7777])
        produce = produce_cmd(t_ms, txs, files)
        inspect = {t["file"]: run("tx", "inspect", f"txs/{t['file']}", expect=None) for t in txs}
        rec["blocks"].append({"height": h, "txs": txs, "build": outs, "files": files, "inspect": inspect,
                              "produce": produce, "result": run(*produce, expect=None), "snap": snapshot(run, addr)})
        print(f"block {h}: {len(txs)} txs, root {rec['blocks'][-1]['snap']['state_root'].split()[-1]}")
    run("dev", "export", "--out", "journal.hex")
    rec["verify"] = run("verify-state")["out"]
    rec["db_sha"] = sha_tree(os.path.join(out, "home", "data"))
    with open(os.path.join(out, "record.json"), "w") as f:
        json.dump(rec, f, indent=1, sort_keys=True)
    print(f"captured {a.blocks} blocks in {out}")
    return 0


def load(dir_):
    with open(os.path.join(dir_, "record.json")) as f:
        return json.load(f)


def same(what, want, got, diffs):
    if want != got:
        diffs.append(what)
        print(f"DIFF {what}\n  baseline : {json.dumps(want)[:600]}\n  candidate: {json.dumps(got)[:600]}")


def compare(a):
    d, w = os.path.abspath(a.dir), os.path.abspath(a.work)
    rec = load(d)
    db_before = sha_tree(os.path.join(d, "home", "data"))
    same("preserved baseline database before", rec["db_sha"], db_before, diffs := [])
    prepare_run_dir(w, None)
    os.makedirs(os.path.join(w, "home"))
    for x in ("keys", "dev"):
        shutil.copytree(os.path.join(d, "seed", x), os.path.join(w, "home", x))
    run = Run(a.yap, w)
    addr = rec["addr"]
    for f, g in rec["genesis"].items():
        run(*g["args"], "--out", f)
        same(f"genesis file {f}", open(os.path.join(d, f)).read(), open(os.path.join(w, f)).read(), diffs)
        same(f"genesis hash {f}", g["hash"], run("genesis", "hash", os.path.join(d, f))["out"].replace(d + "/", ""), diffs)
        same(f"genesis validate {f}", g["validate"], run("genesis", "validate", f, expect=None), diffs)
    same("dev init", rec["init"], run("dev", "init", "--genesis", "genesis.json")["out"], diffs)
    same("state at height 0", rec["snap0"], snapshot(run, addr), diffs)
    for b in rec["blocks"]:
        h = b["height"]
        outs, files = build_txs(run, b["txs"])
        same(f"block {h}: transaction build output (tx ids, fields)", b["build"], outs, diffs)
        same(f"block {h}: transaction and signing-payload bytes", b["files"], files, diffs)
        # Identical inputs: the produce below always reads the baseline's files.
        for path, content in b["files"].items():
            if content is not None:
                shutil.copyfile(os.path.join(d, path), os.path.join(w, path))
        same(f"block {h}: tx inspect", b["inspect"],
             {t["file"]: run("tx", "inspect", f"txs/{t['file']}", expect=None) for t in b["txs"]}, diffs)
        same(f"block {h}: produce (block hash, tx root, included/excluded with codes)", b["result"],
             run(*b["produce"], expect=None), diffs)
        same(f"block {h}: state (root, supply, minted, burned, balances, nonces, rewards)", b["snap"],
             snapshot(run, addr), diffs)
    run("dev", "export", "--out", "journal.hex")
    same("exported journal (every block, byte for byte)", open(os.path.join(d, "journal.hex")).read(),
         open(os.path.join(w, "journal.hex")).read(), diffs)
    same("verify-state", rec["verify"], run("verify-state")["out"], diffs)
    # Replay of the baseline journal from genesis by the candidate.
    rw = os.path.join(w, "replay")
    prepare_run_dir(rw, None)
    shutil.copytree(os.path.join(d, "seed"), os.path.join(rw, "home"))
    shutil.copyfile(os.path.join(d, "genesis.json"), os.path.join(rw, "genesis.json"))
    shutil.copyfile(os.path.join(d, "journal.hex"), os.path.join(rw, "journal.hex"))
    rr = Run(a.yap, rw)
    rr("dev", "init", "--genesis", "genesis.json")
    rr("dev", "replay", "--journal", "journal.hex")
    same("replay of the baseline journal: state", rec["blocks"][-1]["snap"], snapshot(rr, addr), diffs)
    same("replay of the baseline journal: verify-state", rec["verify"], rr("verify-state")["out"], diffs)
    same("preserved baseline database after", db_before, sha_tree(os.path.join(d, "home", "data")), diffs)
    n_tx = sum(len(b["txs"]) for b in rec["blocks"])
    print(f"compare: {len(rec['blocks'])} blocks, {n_tx} transactions, {len(rec['genesis'])} genesis files: "
          f"{'IDENTICAL' if not diffs else f'{len(diffs)} DIFFERENCES'}")
    return 1 if diffs else 0


def lockstep(runs, rng, h0, n, addr, prefix, t_ms, diffs, label):
    """Extends every run's chain by n blocks on identical inputs; all outputs must agree."""
    first = runs[0]
    for h in range(h0 + 1, h0 + n + 1):
        nonces, balances = account_state(first, addr)
        txs = plan_block(rng, h, nonces, balances, addr, prefix)
        t_ms += rng.choice([1, 2000, 5000])
        results = []
        for r in runs:
            outs, files = build_txs(r, txs)
            produce = produce_cmd(t_ms, txs, files)
            results.append({"build": outs, "files": files, "produce": produce, "result": r(*produce, expect=None),
                            "snap": snapshot(r, addr)})
        for r, res in zip(runs[1:], results[1:]):
            same(f"{label} block {h} ({os.path.basename(r.cwd)} vs {os.path.basename(first.cwd)})",
                 results[0], res, diffs)
        print(f"{label} block {h}: {len(txs)} txs, root {results[0]['snap']['state_root'].split()[-1]}")
    return t_ms


def last_time(run):
    return int(run.kv("query", "chain")["last_time_ms"])


def dbcompat(a):
    d, w = os.path.abspath(a.dir), os.path.abspath(a.work)
    rec = load(d)
    addr, diffs = rec["addr"], []
    preserved = os.path.join(d, "home")
    before = sha_tree(preserved)
    same("preserved baseline database before", rec["db_sha"], sha_tree(os.path.join(preserved, "data")), diffs)
    final = rec["blocks"][-1]["snap"]
    # 1. The candidate opens a copy of the C++20 database.
    ca, ba = os.path.join(w, "cand"), os.path.join(w, "base")
    prepare_run_dir(ca, preserved)
    prepare_run_dir(ba, preserved)
    cand, base = Run(a.yap, ca), Run(a.base, ba)
    same("candidate verify-state on the baseline database", rec["verify"], cand("verify-state")["out"], diffs)
    same("candidate queries on the baseline database", final, snapshot(cand, addr), diffs)
    # 2. Both continue the same chain on identical inputs.
    rng = random.Random(a.seed + 1)
    t_ms = last_time(cand)
    h = rec["n_blocks"]
    t_ms = lockstep([base, cand], rng, h, a.blocks, addr, "c", t_ms, diffs, "continue")
    h += a.blocks
    # 3. Persistence: fresh processes re-open both databases.
    v_base = base("verify-state")["out"]
    same("candidate verify-state after continuing (re-opened)", v_base, cand("verify-state")["out"], diffs)
    same("candidate state after re-open", snapshot(base, addr), snapshot(cand, addr), diffs)
    for r in (base, cand):
        r("dev", "export", "--out", "journal.hex")
    same("journals after continuing", open(os.path.join(ba, "journal.hex")).read(),
         open(os.path.join(ca, "journal.hex")).read(), diffs)
    # 4. The baseline opens the database the candidate wrote, and both extend it once more.
    rev = os.path.join(w, "reverse")
    prepare_run_dir(rev, os.path.join(ca, "home"))
    back = Run(a.base, rev)
    same("baseline verify-state on the candidate's database", v_base, back("verify-state")["out"], diffs)
    same("baseline queries on the candidate's database", snapshot(base, addr), snapshot(back, addr), diffs)
    lockstep([back, cand], rng, h, 3, addr, "r", t_ms, diffs, "reverse")
    same("candidate verify-state at the end", back("verify-state")["out"], cand("verify-state")["out"], diffs)
    same("preserved baseline database after (every file)", before, sha_tree(preserved), diffs)
    print(f"dbcompat: opened the {rec['n_blocks']}-block baseline database, {a.blocks} + 3 lockstep blocks: "
          f"{'IDENTICAL' if not diffs else f'{len(diffs)} DIFFERENCES'}")
    return 1 if diffs else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--yap", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--blocks", type=int, default=40)
    c.add_argument("--seed", type=int, default=20260)
    m = sub.add_parser("compare")
    m.add_argument("--dir", required=True)
    m.add_argument("--yap", required=True)
    m.add_argument("--work", required=True)
    b = sub.add_parser("dbcompat")
    b.add_argument("--dir", required=True)
    b.add_argument("--base", required=True)
    b.add_argument("--yap", required=True)
    b.add_argument("--work", required=True)
    b.add_argument("--blocks", type=int, default=12)
    b.add_argument("--seed", type=int, default=20260)
    a = ap.parse_args()
    try:
        return {"capture": capture, "compare": compare, "dbcompat": dbcompat}[a.mode](a)
    except Diff as e:
        print(f"FAILED: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
