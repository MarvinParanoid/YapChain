#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end acceptance of the local YapChain workflow (M1.4e).

Every step runs the real `yap` binary in its own process; the test never calls library code.
Expected amounts are computed here from the protocol rules (economics.md §4, protocol.md §9.3), and
system-account addresses come from the independent Python reference (tools/refgen)."""
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cli"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "refgen"))
from clilib import Cli, check, password_file, tmpdir  # noqa: E402
import bech32m_ref  # noqa: E402

YAP = 1_000_000
FEE = 1_000
VALIDATOR_PK = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"


def system_address(name):
    t = b"YAP/sys-account/v1"
    return bech32m_ref.encode_address("tyap", hashlib.sha256(bytes([len(t)]) + t + name).digest())


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def step(n, text):
    print(f"[{n:2}] {text}")


def main(yap):
    d = tmpdir()
    try:
        pw = password_file(d, "correct horse battery staple")
        f = lambda n: os.path.join(d, n)  # noqa: E731
        a = Cli(yap, f("node-a"), pw)

        step(1, "create Alice and Bob")
        alice = a.kv("keys", "create", "alice")["address"]
        bob = a.kv("keys", "create", "bob")["address"]
        val = a.kv("keys", "create", "validator-rewards")["address"]

        step(2, "create a genesis with Alice's initial balance")
        a.run("genesis", "new", "--chain-id", "yapchain-e2e-1", "--account", f"{alice}=100YAP",
              "--validator", f"{VALIDATOR_PK}:10:{val}:validator-1", "--out", f("genesis.json"))
        g = a.kv("genesis", "hash", f("genesis.json"))

        step(3, "initialise the dev chain")
        init = a.kv("dev", "init", "--genesis", f("genesis.json"))
        check(init["created"] == "yes" and init["height"] == "0", f"init: {init}")

        step(4, "check the genesis state root")
        check(init["state_root"] == g["initial_state_root"], "init root == genesis hash tool root")
        check(a.kv("dev", "state-root")["state_root"] == g["initial_state_root"], "state-root at height 0")
        check(a.kv("query", "chain")["total_supply"] == str(100 * YAP), "genesis supply")

        step(5, "sign a Transfer Alice -> Bob")
        t1 = a.kv("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "25YAP", "--out", f("t1.hex"))
        check(t1["nonce"] == "0" and t1["expires_at_height"] == "100", f"nonce/expiry from local state: {t1}")
        check(a.kv("tx", "inspect", f("t1.hex"))["signature"] == "valid", "signed transfer verifies")

        step(6, "produce a block")
        b1 = a.kv("dev", "produce", "--tx", f("t1.hex"), "--require-all")
        check(b1["height"] == "1" and b1["included"] == t1["tx_id"], f"block 1: {b1}")

        step(7, "check balances and nonces")
        alice_acct = a.kv("query", "account", alice)
        check(alice_acct["balance_yip"] == str(100 * YAP - 25 * YAP - FEE) and alice_acct["nonce"] == "1",
              f"alice: {alice_acct}")
        bob_acct = a.kv("query", "account", bob)
        check(bob_acct["balance_yip"] == str(25 * YAP) and bob_acct["nonce"] == "0", f"bob: {bob_acct}")

        step(8, "sign a Burn")
        t2 = a.kv("tx", "burn", "--from", "alice", "--amount", "2.5YAP", "--out", f("t2.hex"))
        check(t2["nonce"] == "1", "second nonce from local state")

        step(9, "produce the next block")
        b2 = a.kv("dev", "produce", "--tx", f("t2.hex"), "--require-all")
        check(b2["height"] == "2" and b2["prev_block_hash"] == b1["block_hash"], f"block 2: {b2}")

        step(10, "check total_supply and total_burned")
        minted = 100 * YAP + 2 * 2 * YAP                       # genesis + two block rewards
        burned = FEE // 2 + (2_500_000 + FEE // 2)              # fee burns + the Burn itself
        chain = a.kv("query", "chain")
        check(chain["total_minted"] == str(minted) and chain["total_burned"] == str(burned) and
              chain["total_supply"] == str(minted - burned), f"supply: {chain}")
        # Full bookkeeping: every yip is in a known account.
        expected = {
            alice: 100 * YAP - 25 * YAP - FEE - 2_500_000 - FEE,
            bob: 25 * YAP,
            val: 1_200_000,                                     # block 2 rewards the height-1 signers
            system_address(b"treasury"): (200_000 + 1_200_000) + 200_000 + 2 * (FEE // 2),
            system_address(b"rewards-pool"): 2 * 600_000,
        }
        balances = {addr: int(a.kv("query", "balance", addr)["balance_yip"]) for addr in expected}
        check(balances == expected, f"balances {balances} != {expected}")
        check(sum(balances.values()) == minted - burned, "all supply accounted for")
        roots_a = [a.kv("dev", "state-root", "--height", h)["state_root"] for h in range(3)]

        step(11, "end the process (every command above ran in its own process)")
        step(12, "open the database again")
        check(a.kv("verify-state")["status"] == "ok", "verify-state after reopening")

        step(13, "check all balances and the state root after reopening")
        check({addr: int(a.kv("query", "balance", addr)["balance_yip"]) for addr in expected} == expected,
              "balances after reopening")
        check(a.kv("query", "chain")["state_root"] == roots_a[2] == b2["state_root"], "state root after reopening")

        step(14, "replay the journal into a new database")
        a.run("dev", "export", "--out", f("journal.hex"))
        b = Cli(yap, f("node-b"), pw)
        b.run("dev", "init", "--genesis", f("genesis.json"), "--producer-pubkey", init["producer"])
        rep = b.kv("dev", "replay", "--journal", f("journal.hex"))
        check(rep["imported"] == "2" and rep["skipped"] == "0", f"replay: {rep}")

        step(15, "compare all roots")
        roots_b = [b.kv("dev", "state-root", "--height", h)["state_root"] for h in range(3)]
        check(roots_a == roots_b, f"roots differ: {roots_a} vs {roots_b}")
        check(b.kv("query", "chain") == a.kv("query", "chain"), "identical chain summaries")
        check(b.kv("verify-state")["status"] == "ok", "replayed database verifies")

        step(16, "re-import and invalid blocks")
        a.run("dev", "block", "--height", "2", "--out", f("block2.hex"))
        before = b.kv("query", "chain")
        out = b.run("dev", "import", f("block2.hex"), expect=2)
        check("already committed" in out.stderr, "re-import refused")
        b.run("dev", "replay", "--journal", f("journal.hex"))  # identical blocks: skipped
        check(b.kv("query", "chain") == before, "no second emission after re-import or replay")
        raw = open(f("block2.hex")).read().strip()
        node_c = Cli(yap, f("node-c"), pw)
        node_c.run("dev", "init", "--genesis", f("genesis.json"), "--producer-pubkey", init["producer"])
        a.run("dev", "block", "--height", "1", "--out", f("block1.hex"))
        node_c.run("dev", "import", f("block2.hex"), expect=2)            # height gap
        tampered = raw[:100] + ("0" if raw[100] != "0" else "1") + raw[101:]
        open(f("tampered.hex"), "w").write(tampered)
        node_c.run("dev", "import", f("block1.hex"))
        c_before = node_c.kv("query", "chain")
        out = node_c.run("dev", "import", f("tampered.hex"), expect=2)   # flipped header byte
        check("rejected" in out.stderr, "tampered block rejected")
        check(node_c.kv("query", "chain") == c_before, "rejected block left no trace")
        node_c.run("dev", "import", f("block2.hex"))
        check(node_c.kv("query", "chain") == before, "genuine block still accepted")

        step(17, "wrong password and damaged keystore")
        key_file = os.path.join(f("node-a"), "keys", "alice.json")
        digest = sha(key_file)
        wrong = password_file(d, "definitely not it")
        out = a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "1", "--out", f("x.hex"),
                    password_file=wrong, expect=4)
        check("wrong password" in out.stderr and sha(key_file) == digest, "wrong password: refused, file untouched")
        check(not os.path.exists(f("x.hex")), "nothing signed")
        original = open(key_file).read()
        os.chmod(key_file, 0o600)
        ct = original.split('"ciphertext": "')[1][:2]
        damaged = original.replace('"ciphertext": "' + ct, '"ciphertext": "' + ("00" if ct != "00" else "11"), 1)
        os.remove(key_file)
        open(key_file, "w").write(damaged)
        os.chmod(key_file, 0o600)
        out = a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "1", "--out", f("x.hex"), expect=4)
        check("corrupted" in out.stderr, "damaged ciphertext refused")
        os.remove(key_file)
        open(key_file, "w").write(original[: len(original) // 2])
        os.chmod(key_file, 0o600)
        a.run("keys", "show", "alice", expect=4)
        os.remove(key_file)
        open(key_file, "w").write(original)
        os.chmod(key_file, 0o600)
        a.run("tx", "transfer", "--from", "alice", "--to", bob, "--amount", "1", "--out", f("x.hex"))  # restored
        print("E2E workflow: OK (17/17 steps)")
    finally:
        shutil.rmtree(d)


if __name__ == "__main__":
    main(sys.argv[1])
