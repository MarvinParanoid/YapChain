# SPDX-License-Identifier: Apache-2.0
"""Independent reference execution of blocks (protocol.md §9, §7.5, §9.3, §9.5; ADR-0021).

Keeps a structured state model and serialises it to raw state entries only to compute the
state root, so it shares no data path with the C++ overlay implementation.
"""
import copy

import economics_ref as econ
import merkle_ref
import state_ref
import tx_ref

U64_MAX = 2**64 - 1


class Overflow(Exception):
    pass


def _add(a, b):
    if a + b > U64_MAX:
        raise Overflow()
    return a + b


def _sub(a, b):
    if b > a:
        raise Overflow()
    return a - b


class Chain:
    def __init__(self, genesis):
        self.g = genesis
        self.chain_id = genesis["chain_id"]
        self.params = dict(genesis["params"])
        self.accounts = {a["account_id"]: {"balance": a["balance"], "nonce": 0} for a in genesis["accounts"]}
        self.validators = {state_ref.consensus_address(v["consensus_pubkey"]): v for v in genesis["validators"]}
        supply = sum(a["balance"] for a in genesis["accounts"])
        self.cs = {"height": 0, "last_time_ms": genesis["genesis_time_ms"], "total_supply": supply,
                   "total_minted": supply, "total_burned": 0}
        static = state_ref.initial_state(genesis)
        self.static = {k: v for k, v in static.items() if k[:1] in (b"\x04", b"\x06")}

    # -------------------------------------------------------------- commitments
    def entries(self):
        st = dict(self.static)
        st[state_ref.key_params()] = tx_ref.encode_params(self.params)
        st[state_ref.key_chain_state()] = state_ref.enc_chain_state(self.cs)
        for aid, a in self.accounts.items():
            st[state_ref.key_account(aid)] = state_ref.enc_account(a["balance"], a["nonce"])
        for addr, v in self.validators.items():
            st[state_ref.key_validator(addr)] = state_ref.enc_validator(v)
        return st

    def root(self):
        return merkle_ref.state_root(self.entries())

    def check_invariants(self):
        assert sum(a["balance"] for a in self.accounts.values()) == self.cs["total_supply"]
        assert self.cs["total_supply"] == self.cs["total_minted"] - self.cs["total_burned"]
        assert self.cs["total_minted"] <= self.params["max_issuance"]

    # -------------------------------------------------------------- execution
    def _credit(self, accounts, aid, amount):
        if amount == 0:
            return  # ADR-0021
        a = accounts.setdefault(aid, {"balance": 0, "nonce": 0})
        a["balance"] = _add(a["balance"], amount)

    def execute_tx(self, tx_bytes, height):
        code = tx_ref.validate(tx_bytes, height, self.chain_id, self.accounts, self.params)
        if code != "OK":
            return code
        body, _ = tx_ref.decode_tx(tx_bytes)
        accounts = copy.deepcopy(self.accounts)
        cs = dict(self.cs)
        sender = tx_ref.account_id(body["sender_pubkey"])
        p = body["payload"]
        fee = body["fee"]
        try:
            s = accounts[sender]
            s["balance"] = _sub(s["balance"], _add(p["amount"], fee))
            if p["type"] == "transfer":
                self._credit(accounts, p["to"], p["amount"])
            else:  # burn (claims never reach this point in M1.2)
                cs["total_burned"] = _add(cs["total_burned"], p["amount"])
                cs["total_supply"] = _sub(cs["total_supply"], p["amount"])
            burn_part = fee * self.params["fee_burn_bps"] // 10000
            self._credit(accounts, econ.TREASURY, fee - burn_part)
            cs["total_burned"] = _add(cs["total_burned"], burn_part)
            cs["total_supply"] = _sub(cs["total_supply"], burn_part)
            accounts[sender]["nonce"] = _add(accounts[sender]["nonce"], 1)
        except Overflow:
            return "ERR_OVERFLOW"
        self.accounts, self.cs = accounts, cs  # atomic commit of the transaction
        return "OK"

    def execute_block(self, height, time_ms, last_commit, txs):
        """last_commit: list of (address, power, flag). Returns (codes, root)."""
        assert height == self.cs["height"] + 1 and time_ms >= self.cs["last_time_ms"]
        assert height > 1 or not last_commit
        codes = [self.execute_tx(t, height) for t in txs]
        vals = {a: (v["power"], v["reward_account"]) for a, v in self.validators.items()}
        credits, minted = econ.emission(self.params, self.cs["total_minted"], last_commit, vals)
        for aid, amount in credits.items():
            self._credit(self.accounts, aid, amount)
        self.cs["total_minted"] += minted
        self.cs["total_supply"] += minted
        self.cs["height"] = height
        self.cs["last_time_ms"] = time_ms
        self.check_invariants()
        return codes, self.root()
