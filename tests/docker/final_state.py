# SPDX-License-Identifier: Apache-2.0
"""Final state verification of the container acceptance suite, independent of shutdown timing (M2.7.2).

The suite brings the network to a quiescent height F before it shuts it down: two of four validators are
stopped (quorum lost), and the remaining nodes must report one height F, both CometBFT's and the
application's, unchanged over a whole observation window. The independent references are extended
through F from consensus block data only:

- livenet.Model: balances, nonces, issuance, burns, rewards (the suite's existing model);
- ReferenceRoots: the reference executor of tools/refgen (exec_ref, merkle_ref) applied to the same
  blocks, giving the expected state root at every height, F included (no Header(F+1) exists).

After shutdown every durable database must be at a height <= F that both references cover, with exactly
the reference root of its own height. This module holds only the decisions (no Docker), so
tests/docker/test_final_state.py checks them deterministically.
"""
import base64
import calendar
import json
import os
import sys
import time

REFGEN = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools", "refgen")
sys.path.insert(0, REFGEN)
import bech32m_ref  # noqa: E402
import exec_ref  # noqa: E402
import state_ref  # noqa: E402


class FinalStateError(AssertionError):
    pass


def time_ms(rfc3339):
    """CometBFT block time (RFC 3339, up to nanoseconds, UTC) -> Unix milliseconds, truncated like the ABCI
    adapter (seconds * 1000 + nanos / 1e6)."""
    s = rfc3339.rstrip("Z")
    main, _, frac = s.partition(".")
    secs = calendar.timegm(time.strptime(main, "%Y-%m-%dT%H:%M:%S"))
    nanos = int((frac + "000000000")[:9]) if frac else 0
    return secs * 1000 + nanos // 1_000_000


class ReferenceRoots:
    """Expected state roots from the genesis document and consensus blocks only (never from a node's
    application state)."""

    def __init__(self, yap_genesis):
        g = state_ref.parse_genesis_json(json.dumps(yap_genesis))
        self.chain = exec_ref.Chain(g)
        self.roots = {0: self.chain.root().hex()}

    @property
    def top(self):
        return max(self.roots)

    def apply_block(self, block, valset):
        """block: CometBFT /block result["block"]; valset: validator addresses (hex) in CometBFT order.
        Returns the reference root after this height."""
        hdr = block["header"]
        h = int(hdr["height"])
        if h != self.top + 1:
            raise FinalStateError(f"reference roots: block {h} after height {self.top}")
        votes = []
        if h > 1:
            sigs = block["last_commit"]["signatures"]
            if len(sigs) != len(valset):
                raise FinalStateError(f"height {h}: {len(sigs)} signatures for {len(valset)} validators")
            for i, s in enumerate(sigs):
                addr = bytes.fromhex(valset[i])
                if addr not in self.chain.validators:
                    raise FinalStateError(f"height {h}: validator {valset[i]} is not in the genesis")
                votes.append((addr, self.chain.validators[addr]["power"], int(s["block_id_flag"])))
        txs = [base64.b64decode(t) for t in (block["data"]["txs"] or [])]
        codes, root = self.chain.execute_block(h, time_ms(hdr["time"]), votes, txs)
        if any(c != "OK" for c in codes):
            raise FinalStateError(f"height {h}: the reference rejects a committed transaction: {codes}")
        self.roots[h] = root.hex()
        return self.roots[h]


def cross_check(model_state, chain, hrp="tyap"):
    """The suite's balance model and the reference executor, both built from the same blocks, must agree
    on every balance, nonce, the issuance and the burns (two independent implementations)."""
    errors = []
    for addr, bal in model_state["bal"].items():
        aid = bech32m_ref.decode_address(addr.encode(), hrp)
        got = chain.accounts.get(aid, {"balance": 0, "nonce": 0})
        if got["balance"] != bal:
            errors.append(f"{addr}: model balance {bal} != reference {got['balance']}")
        if got["nonce"] != model_state["nonce"].get(addr, 0):
            errors.append(f"{addr}: model nonce {model_state['nonce'].get(addr, 0)} != reference {got['nonce']}")
    if chain.cs["total_minted"] != model_state["minted"] or chain.cs["total_burned"] != model_state["burned"]:
        errors.append(f"minted/burned: model {model_state['minted']}/{model_state['burned']} != reference "
                      f"{chain.cs['total_minted']}/{chain.cs['total_burned']}")
    if errors:
        raise FinalStateError("model and reference executor disagree: " + "; ".join(errors))


def wait_quiescent(observe, window, deadline_s, poll=0.5, clock=time.monotonic, sleep=time.sleep, min_obs=3):
    """observe() -> {node: (consensus height, application height)}. Quiescent at F when every node reports
    (F, F) and every observation over at least `window` seconds (and at least `min_obs` observations) is
    identical; a single unchanged observation is never enough. Raises FinalStateError at the deadline.
    Returns (F, observations, seconds observed)."""
    t0 = clock()
    deadline = t0 + deadline_s
    ref, since, count, history = None, None, 0, []
    while True:
        obs = observe()
        now = clock()
        history.append((round(now - t0, 2), obs))
        heights = {x for pair in obs.values() for x in pair}
        if len(heights) == 1 and obs == ref:
            count += 1
            if count >= min_obs and now - since >= window:
                return heights.pop(), count, round(now - since, 2)
        else:
            ref, since, count = (obs if len(heights) == 1 else None), now, 1
        if now >= deadline:
            raise FinalStateError(f"no quiescent height within {deadline_s}s (window {window}s); "
                                  f"last observations {history[-5:]}")
        sleep(poll)


def confirm_still(observe, F, window, poll=0.5, clock=time.monotonic, sleep=time.sleep, min_obs=3):
    """Every observation during `window` seconds must still be (F, F) on every node: any block committed
    after quiescence fails at once."""
    t0 = clock()
    count = 0
    while True:
        obs = observe()
        bad = {n: hh for n, hh in obs.items() if hh != (F, F)}
        if bad:
            raise FinalStateError(f"block production after quiescence at {F}: {bad} "
                                  f"({clock() - t0:.1f}s into the confirmation window)")
        count += 1
        if count >= min_obs and clock() - t0 >= window:
            return count
        sleep(poll)


def verify_final(durable, F, model_heights, ref_roots):
    """durable: {node: (height, root hex)} from verify-state of every stopped database. Every height must
    be <= F, covered by the model and by the reference roots, and the root must equal the reference root
    of that node's own height; nodes at one height must have one root. Returns the lowest durable height.
    Raises FinalStateError listing every violation with the context needed to diagnose it."""
    errors = []
    model_top = max(model_heights) if model_heights else None
    for n, (h, root) in sorted(durable.items()):
        if h > F:
            errors.append(f"{n}: durable height {h} exceeds the quiescent height {F} (a block was committed "
                          f"after quiescence; model covers up to {model_top}, reference up to {max(ref_roots)})")
            continue
        if h not in model_heights:
            errors.append(f"{n}: durable height {h} not covered by the independent model (covers up to {model_top})")
        if h not in ref_roots:
            errors.append(f"{n}: durable height {h} not covered by the reference roots (up to {max(ref_roots)})")
        elif root.lower() != ref_roots[h]:
            errors.append(f"{n}: durable root at {h} {root} != reference root {ref_roots[h]}")
    by_height = {}
    for n, (h, root) in durable.items():
        by_height.setdefault(h, {})[n] = root.lower()
    for h, rs in sorted(by_height.items()):
        if len(set(rs.values())) != 1:
            errors.append(f"height {h}: nodes have different durable roots {rs}")
    if errors:
        raise FinalStateError(f"final state (F={F}, durable {dict(sorted(durable.items()))}): " + "; ".join(errors))
    return min(h for h, _ in durable.values())
