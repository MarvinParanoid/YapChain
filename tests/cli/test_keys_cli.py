#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CLI tests for `yap keys` and `yap addr` (real binary)."""
import hashlib
import json
import os
import shutil
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from clilib import Cli, check, password_file, tmpdir  # noqa: E402


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main(yap):
    d = tmpdir()
    try:
        pw = password_file(d, "correct horse battery staple")
        cli = Cli(yap, os.path.join(d, "home"), pw)

        alice = cli.kv("keys", "create", "alice")
        check(alice["address"].startswith("tyap1"), "address prefix")
        bob = cli.kv("keys", "create", "bob")
        check(alice["address"] != bob["address"], "distinct keys")
        listed = cli.run("keys", "list").stdout.split()
        check(listed == ["alice", alice["address"], "bob", bob["address"]], f"list: {listed}")
        shown = cli.kv("keys", "show", "alice")
        check(shown["public_key"] == alice["public_key"] and shown["purpose"] == "account", "show")

        key_file = os.path.join(d, "home", "keys", "alice.json")
        check(stat.S_IMODE(os.stat(key_file).st_mode) == 0o600, "key file mode 0600")
        check(stat.S_IMODE(os.stat(os.path.dirname(key_file)).st_mode) == 0o700, "keys dir mode 0700")
        doc = json.load(open(key_file))
        check(set(doc) == {"version", "purpose", "kdf", "opslimit", "memlimit", "salt", "nonce", "ciphertext",
                           "public_key", "address"}, "keystore fields")
        check(doc["kdf"] == "argon2id" and doc["opslimit"] == "3" and doc["memlimit"] == "268435456",
              "MODERATE Argon2id parameters")
        for out in (cli.run("keys", "show", "alice").stdout, cli.run("keys", "list").stdout):
            check(doc["ciphertext"] not in out, "no secret material in output")

        # Duplicate names never overwrite; invalid names are rejected.
        before = sha(key_file)
        cli.run("keys", "create", "alice", expect=2)
        check(sha(key_file) == before, "existing key untouched")
        cli.run("keys", "create", "../evil", expect=2)

        # Too-open permissions are refused.
        os.chmod(key_file, 0o644)
        cli.run("keys", "show", "alice", expect=4)
        os.chmod(key_file, 0o600)

        # Password from stdin when no password file and no terminal.
        cli_stdin = Cli(yap, os.path.join(d, "home"))
        carol = cli_stdin.kv("keys", "create", "carol", stdin="another long password\n")
        check(carol["address"].startswith("tyap1"), "create with stdin password")
        cli_stdin.run("keys", "create", "dave", stdin="short\n", expect=4)

        # Address validation.
        v = cli.kv("addr", "validate", alice["address"])
        check(v["valid"] == "yes" and v["address"] == alice["address"], "valid address")
        cli.run("addr", "validate", alice["address"].upper())  # all-uppercase is valid Bech32m
        cli.run("addr", "validate", alice["address"][:-1] + ("q" if alice["address"][-1] != "q" else "p"), expect=2)
        cli.run("addr", "validate", "yap1" + alice["address"][5:], expect=2)
        cli.run("addr", "validate", "nonsense", expect=2)
        cli.run("keys", expect=1)  # usage error
        print("keys CLI: OK")
    finally:
        shutil.rmtree(d)


if __name__ == "__main__":
    main(sys.argv[1])
