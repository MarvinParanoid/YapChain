#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CLI tests for `yap genesis`, `yap dev`, `yap query` and `yap verify-state` (real binary)."""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from clilib import Cli, check, password_file, tmpdir  # noqa: E402

VALIDATOR_PK = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"  # RFC 8032 test key


def main(yap, corrupt):
    d = tmpdir()
    try:
        pw = password_file(d, "correct horse battery staple")
        f = lambda n: os.path.join(d, n)  # noqa: E731
        a = Cli(yap, f("home-a"), pw)
        alice = a.kv("keys", "create", "alice")["address"]
        bob = a.kv("keys", "create", "bob")["address"]

        # genesis new / validate / hash
        a.run("genesis", "new", "--account", f"{alice}=50YAP", "--validator", f"{VALIDATOR_PK}:10:{bob}:val-1",
              "--out", f("genesis.json"))
        a.run("genesis", "new", "--account", f"{alice}=50YAP", "--validator", f"{VALIDATOR_PK}:10:{bob}:v",
              "--out", f("genesis.json"), expect=2)  # never overwrites
        v = a.kv("genesis", "validate", f("genesis.json"))
        check(v["valid"] == "yes" and v["total_supply"] == "50.000000 YAP (50000000 yip)", f"validate: {v}")
        gh = a.kv("genesis", "hash", f("genesis.json"))
        bad = open(f("genesis.json")).read().replace('"initial_height": "1"', '"initial_height": "2"')
        open(f("bad-genesis.json"), "w").write(bad)
        a.run("genesis", "validate", f("bad-genesis.json"), expect=2)
        a.run("genesis", "new", "--validator", f"{'00' * 32}:10:{bob}:v", "--out", f("g2.json"), expect=2)  # small-order key

        # dev init: creates the producer key; re-init is a no-op; a different genesis is refused
        init = a.kv("dev", "init", "--genesis", f("genesis.json"))
        check(init["created"] == "yes" and init["state_root"] == gh["initial_state_root"], f"init: {init}")
        check(a.kv("dev", "init", "--genesis", f("genesis.json"))["created"].startswith("no"), "re-init is a no-op")
        a.run("genesis", "new", "--time-ms", "1", "--account", f"{alice}=50YAP",
              "--validator", f"{VALIDATOR_PK}:10:{bob}:val-1", "--out", f("other.json"))
        a.run("dev", "init", "--genesis", f("other.json"), expect=2)
        check(a.kv("query", "chain")["genesis_hash"] == gh["genesis_hash"], "existing chain untouched")
        producer = init["producer"]

        # produce blocks; nonce and expiry come from the local state
        for i in range(3):
            a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", str(1000 + i), "--out", f(f"t{i}.hex"))
            p = a.kv("dev", "produce", "--tx", f(f"t{i}.hex"), "--require-all")
            check(p["height"] == str(i + 1) and p["txs"] == "1", f"produce {i}: {p}")
        # an invalid candidate is reported, never silently dropped; --require-all refuses it
        a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "1", "--nonce", "0", "--out", f("stale.hex"))
        out = a.run("dev", "produce", "--tx", f("stale.hex"), "--require-all", expect=2)
        check("ERR_NONCE" in out.stderr, "require-all explains the failing transaction")
        out = a.run("dev", "produce", "--tx", f("stale.hex")).stdout
        check("excluded: " in out and "ERR_NONCE" in out, "excluded tx is reported with its code")

        # Bad block times are rejected inputs (exit 2), never node failures, and never brick the chain.
        out = a.run("dev", "produce", "--time-ms", "5", expect=2)
        check("invalid block time" in out.stderr, "past time rejected as bad time")
        a.run("dev", "produce", "--time-ms", "18446744073709551615", expect=2)
        a.run("dev", "produce")  # still producing
        acct = a.kv("query", "account", alice)
        check(acct["nonce"] == "3" and acct["height"] == "5", f"account after 5 blocks: {acct}")
        chain_a = a.kv("query", "chain")
        roots_a = [a.kv("dev", "state-root", "--height", h)["state_root"] for h in range(6)]
        check(roots_a[0] == gh["initial_state_root"], "height 0 root == genesis root")
        check(a.kv("verify-state")["status"] == "ok", "verify-state ok")

        # export / import / replay into a second, import-only node
        a.run("dev", "export", "--out", f("journal.hex"))
        a.run("dev", "block", "--height", "2", "--out", f("block2.hex"))
        b = Cli(yap, f("home-b"), pw)
        b.run("dev", "init", "--genesis", f("genesis.json"), "--producer-pubkey", producer)
        b.run("dev", "import", f("block2.hex"), expect=2)  # height gap
        a.run("dev", "block", "--height", "1", "--out", f("block1.hex"))
        b.run("dev", "import", f("block1.hex"))
        before = b.kv("query", "chain")
        b.run("dev", "import", f("block1.hex"), expect=2)  # re-import: rejected ...
        check(b.kv("query", "chain") == before, "... with no second emission")
        # tampered blocks
        raw = open(f("block2.hex")).read().strip()
        for pos in (40, len(raw) - 10, len(raw) // 2):
            t = list(raw)
            t[pos] = "0" if t[pos] != "0" else "1"
            open(f("bad.hex"), "w").write("".join(t))
            b.run("dev", "import", f("bad.hex"), expect=2)
            check(b.kv("query", "chain") == before, "tampered block leaves the state unchanged")
        r = b.kv("dev", "replay", "--journal", f("journal.hex"))
        check(r["imported"] == "4" and r["skipped"] == "1", f"replay: {r}")
        roots_b = [b.kv("dev", "state-root", "--height", h)["state_root"] for h in range(6)]
        check(roots_a == roots_b, "replayed roots equal the original chain")
        check(b.kv("query", "chain") == chain_a, "replayed chain summary equal")

        # Concurrent writers (audit M1.5): two imports of competing height-2 blocks at the same time
        # must never both succeed, and the database must stay consistent.
        a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "77", "--out", f("race.hex"))
        alt = Cli(yap, f("home-alt"), pw)
        shutil.copytree(f("home-a"), f("home-alt"))
        alt.run("dev", "produce", "--tx", f("race.hex"))  # a different block 6 on a copy of the chain
        alt.run("dev", "block", "--height", "6", "--out", f("alt5.hex"))
        a.run("dev", "produce")
        a.run("dev", "block", "--height", "6", "--out", f("main5.hex"))
        for i in range(12):
            race_home = f(f"race-{i}")
            shutil.copytree(f("home-b"), race_home)
            r = Cli(yap, race_home, pw)
            r.run("dev", "replay", "--journal", f("journal.hex"))  # heights 1..5
            procs = [subprocess.Popen([yap, "--home", race_home, "dev", "import", blk], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) for blk in (f("main5.hex"), f("alt5.hex"))]
            codes = [p.wait() for p in procs]
            check(codes.count(0) <= 1, f"both competing imports succeeded (run {i})")
            check(r.kv("verify-state")["status"] == "ok", f"database consistent after the race (run {i})")

        # a node trusting a different producer refuses the blocks
        c = Cli(yap, f("home-c"), pw)
        c.run("dev", "init", "--genesis", f("genesis.json"), "--producer-pubkey", "11" * 32, expect=0)
        out = c.run("dev", "import", f("block1.hex"), expect=2)
        check("dev key" in out.stderr, "wrong producer reported")

        # storage errors are not hidden
        acct_id = a.kv("query", "account", alice)["account_id"]
        subprocess.run([corrupt, os.path.join(f("home-b"), "data"), "balance", acct_id], check=True)
        out = b.run("verify-state", expect=3)
        check("state" in out.stderr.lower(), f"verify-state reports corruption: {out.stderr}")
        subprocess.run([corrupt, os.path.join(f("home-a"), "data"), "smt-node"], check=True)
        a.run("verify-state", expect=3)
        subprocess.run([corrupt, os.path.join(f("home-a"), "data"), "schema"], check=True)
        out = a.run("query", "chain", expect=3)
        check("schema" in out.stderr, "schema mismatch reported")
        a.run("dev", "produce", expect=3)
        Cli(yap, f("home-empty"), pw).run("query", "chain", expect=2)  # no chain
        print("dev CLI: OK")
    finally:
        shutil.rmtree(d)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
