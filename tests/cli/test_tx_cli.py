#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CLI tests for `yap tx` (real binary). Signed bytes are checked with the independent Python
reference (tools/refgen): YCS decoding, fields, tx_id and the Ed25519 signature."""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "refgen"))
from clilib import Cli, check, password_file, tmpdir  # noqa: E402
import bech32m_ref  # noqa: E402
import ed25519_ref  # noqa: E402
import tx_ref  # noqa: E402

CHAIN = "yapchain-dev-1"


def read_hex(path):
    return bytes.fromhex(open(path).read().strip())


def main(yap):
    d = tmpdir()
    try:
        pw = password_file(d, "correct horse battery staple")
        cli = Cli(yap, os.path.join(d, "home"), pw)
        alice = cli.kv("keys", "create", "alice")
        bob = cli.kv("keys", "create", "bob")
        f = lambda name: os.path.join(d, name)  # noqa: E731
        common = ["--chain-id", CHAIN, "--nonce", "0", "--expires", "50"]

        # build -> inspect (before signing) -> sign
        b = cli.kv("tx", "build", "--type", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "1.5YAP",
                   "--memo", "for memes", *common, "--out", f("body.hex"))
        check(b["amount"] == "1.500000 YAP (1500000 yip)" and b["fee"] == "0.001000 YAP (1000 yip)", f"build: {b}")
        ins = cli.kv("tx", "inspect", f("body.hex"))
        check(ins["kind"] == "unsigned_body" and ins["to"] == bob["address"] and ins["nonce"] == "0", f"inspect: {ins}")
        s1 = cli.kv("tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("tx1.hex"))
        check(s1["sender"] == alice["address"], "sign shows the body first")

        # Independent verification of the signed bytes.
        raw = read_hex(f("tx1.hex"))
        body, sig = tx_ref.decode_tx(raw)
        check(body["chain_id"] == CHAIN and body["nonce"] == 0 and body["fee"] == 1000 and body["expires_at_height"] == 50,
              "reference decodes header fields")
        check(body["payload"] == {"type": "transfer", "amount": 1_500_000,
                                  "to": bech32m_ref.decode_address(bob["address"].encode(), "tyap")},
              "reference decodes payload")
        check(body["memo"] == b"for memes", "memo bytes")
        check(body["sender_pubkey"].hex() == alice["public_key"], "sender key")
        check(ed25519_ref.verify(body["sender_pubkey"], tx_ref.TX_PREFIX + tx_ref.encode_body(body), sig),
              "reference verifies the Ed25519 signature over the YCS body")
        check(tx_ref.tx_id(raw).hex() == s1["tx_id"], "tx_id matches the reference")
        check(tx_ref.stateless(raw, CHAIN, tx_ref.TESTNET_PARAMS)[0] == "OK", "reference stateless validation")

        # Deterministic: signing the same body again gives identical bytes; one-step transfer too.
        cli.run("tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("tx1b.hex"))
        check(read_hex(f("tx1b.hex")) == raw, "re-signing gives the same bytes")
        cli.run("tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "1500000", "--memo",
                "for memes", *common, "--out", f("tx1c.hex"))
        check(read_hex(f("tx1c.hex")) == raw, "tx transfer == tx build + tx sign (1.5YAP == 1500000 yip)")

        ins = cli.kv("tx", "inspect", f("tx1.hex"))
        check(ins["kind"] == "signed_transaction" and ins["signature"] == "valid", f"inspect signed: {ins}")

        # Burn.
        cli.run("tx", "burn", "--from", "alice", "--amount", "0.000001YAP", *common, "--out", f("burn.hex"))
        bb, bs = tx_ref.decode_tx(read_hex(f("burn.hex")))
        check(bb["payload"] == {"type": "burn", "amount": 1}, "burn payload")

        # Tampering is visible in inspect.
        t = bytearray(raw)
        t[-1] ^= 1
        open(f("bad.hex"), "w").write(bytes(t).hex())
        bad_sig = cli.run("tx", "inspect", f("bad.hex"), expect=2)  # shown, but reported as invalid
        check("signature: INVALID" in bad_sig.stdout, "tampered signature")
        # Output files are never overwritten silently (audit M1.5) ...
        cli.run("tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("tx1.hex"), expect=2)
        check(read_hex(f("tx1.hex")) == raw, "existing output untouched")
        cli.run("--force", "tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("tx1.hex"))  # ... unless forced
        os.symlink(f("victim"), f("link.hex"))
        cli.run("--force", "tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("link.hex"), expect=3)
        check(not os.path.exists(f("victim")), "symlinks are not followed")
        open(f("junk.hex"), "w").write("00ff")
        cli.run("tx", "inspect", f("junk.hex"), expect=2)
        open(f("nothex.hex"), "w").write("xyz")
        cli.run("tx", "inspect", f("nothex.hex"), expect=2)

        # Errors.
        last = bob["address"][-1]
        broken = bob["address"][:-1] + ("q" if last != "q" else "p")  # always differs from the original
        cli.run("tx", "transfer", "--from", "alice", "--to", broken, "--amount", "1", *common,
                "--out", f("x.hex"), expect=2)  # bad address (checksum)
        for amt in ["1.5", "-1", "1e6", "1.0000001YAP", "99999999999999999999999"]:
            cli.run("tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", amt, *common,
                    "--out", f("x.hex"), expect=2)
        # Integer options are strict decimal (audit M1.5: CLI11 read 010 as 8 and accepted 0x10, 0b11, 1_000).
        for opt, val in (("--nonce", "010"), ("--nonce", "0x10"), ("--nonce", "0b11"), ("--expires", "1_000"),
                         ("--expires", "0100")):
            args = ["tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "1", "--chain-id", CHAIN,
                    "--nonce", "0", "--expires", "50", "--out", f("x.hex")]
            args[args.index(opt) + 1] = val
            cli.run(*args, expect=2)
        for bad_chain in ("Bad Id", "x\nnonce: 999", "-yap", "a" * 33):
            cli.run("tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "1", "--chain-id", bad_chain,
                    "--nonce", "0", "--expires", "50", "--out", f("x.hex"), expect=2)
        cli.run("tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "007", *common,
                "--out", f("x.hex"), expect=2)  # leading zeros in amounts
        cli.run("tx", "transfer", "--from", "alice", "--to", alice["address"], "--amount", "1", *common,
                "--out", f("x.hex"), expect=2)  # self transfer
        cli.run("tx", "sign", "--key", "bob", "--in", f("body.hex"), "--out", f("x.hex"), expect=2)  # wrong signer
        wrong = password_file(d, "not the password")
        cli.run("tx", "sign", "--key", "alice", "--in", f("body.hex"), "--out", f("x.hex"), password_file=wrong, expect=4)
        cli.run("tx", "transfer", "--from", "alice", "--to", bob["address"], "--amount", "1", "--out", f("x.hex"),
                expect=2)  # no local chain and no explicit nonce/expiry/chain id
        check(not os.path.exists(f("x.hex")), "no output file on errors")
        print("tx CLI: OK")
    finally:
        shutil.rmtree(d)


if __name__ == "__main__":
    main(sys.argv[1])
