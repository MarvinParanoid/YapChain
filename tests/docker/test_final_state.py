#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regression tests of the final state verification (tests/docker/final_state.py, M2.7.2), without Docker:
a simulated network with a fake monotonic clock.

    test_final_state.py

Covers the failure of containers run 45976275250 (blocks committed after the last ordinary RPC
observation made `model.at(common)` raise KeyError) and the rules that replace it: quiescence over a whole
window, coverage through F, roots per durable height, no block after quiescence.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import final_state as fs  # noqa: E402

FAILS = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        FAILS.append(msg)


def raises(fn, *a, **kw):
    try:
        fn(*a, **kw)
    except fs.FinalStateError as e:
        return str(e)
    return None


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Chain:
    """Simulated nodes: commits happen at given clock times; the application may lag CometBFT."""

    def __init__(self, clock, commits, nodes=("validator0", "validator1", "fullnode0"), app_lag=None):
        self.clock, self.commits, self.nodes = clock, sorted(commits), nodes
        self.app_lag = app_lag or {}

    def height_at(self, t):
        return sum(1 for c in self.commits if c <= t)

    def observe(self):
        h = self.height_at(self.clock())
        out = {}
        for n in self.nodes:
            lag_until = self.app_lag.get(n)
            app = h - 1 if lag_until and self.clock() < lag_until else h
            out[n] = (h, app)
        return out


def roots_upto(F):
    return {h: f"{h:064x}" for h in range(0, F + 1)}


def main():
    # --- the original failure: the model saw only the last ordinary RPC observation
    F, last_rpc = 197, 193
    durable = {"validator0": (197, f"{197:064x}"), "validator1": (197, f"{197:064x}"),
               "validator2": (195, f"{195:064x}"), "validator3": (196, f"{196:064x}"), "fullnode0": (197, f"{197:064x}")}
    err = raises(fs.verify_final, durable, F, set(range(0, last_rpc + 1)), roots_upto(F))
    check(err is not None and "not covered by the independent model (covers up to 193)" in err,
          "coverage only to the last ordinary RPC observation fails explicitly (no KeyError)")
    common = fs.verify_final(durable, F, set(range(0, F + 1)), roots_upto(F))
    check(common == 195, "model extended through F: nodes stopped at different valid heights (195..197) pass")

    # --- missing reference coverage, wrong roots, divergent roots at one height
    err = raises(fs.verify_final, {"a": (10, f"{10:064x}")}, 10, set(range(11)), roots_upto(9))
    check(err is not None and "not covered by the reference roots" in err, "missing reference coverage fails")
    err = raises(fs.verify_final, {"a": (10, "ab" * 32), "b": (9, f"{9:064x}")}, 10, set(range(11)), roots_upto(10))
    check(err is not None and "durable root at 10" in err and "!= reference root" in err, "an incorrect durable root fails")
    err = raises(fs.verify_final, {"a": (10, "AB" * 32), "b": (10, "cd" * 32)}, 10, set(range(11)), {10: "ab" * 32})
    check(err is not None and "different durable roots" in err, "different roots at one height fail")
    check(fs.verify_final({"a": (10, "AB" * 32)}, 10, set(range(11)), {10: "ab" * 32}) == 10, "root comparison ignores hex case")

    # --- unexpected block production after quiescence
    err = raises(fs.verify_final, {"a": (11, f"{11:064x}"), "b": (10, f"{10:064x}")}, 10, set(range(12)), roots_upto(11))
    check(err is not None and "exceeds the quiescent height 10" in err and "a:" in err,
          "a durable height above F fails with diagnostics, even when covered")

    # --- quiescence: commits continue for a while after the two validators stop (different speeds)
    c = Clock()
    commits = [c.t - 50 + i for i in range(50)] + [c.t + 0.4, c.t + 1.3, c.t + 2.2]  # blocks after the last RPC look
    chain = Chain(c, commits)
    F, nobs, span = fs.wait_quiescent(chain.observe, 10.0, 120.0, clock=c, sleep=c.sleep)
    check(F == 53 and span >= 10.0 and nobs >= 3, f"quiescent at the last commit F={F} after {span}s, {nobs} observations")

    # a single unchanged observation is never enough (window and count)
    c = Clock()
    chain = Chain(c, [c.t - 5, c.t - 4])
    calls = []
    F, nobs, span = fs.wait_quiescent(lambda: calls.append(1) or chain.observe(), 10.0, 120.0, poll=5.0,
                                      clock=c, sleep=c.sleep)
    check(len(calls) >= 3 and span >= 10.0, f"long poll interval still needs >= 3 observations over the window ({len(calls)})")

    # the application lagging CometBFT is not quiescent until it has committed F
    c = Clock()
    chain = Chain(c, [c.t - 3], app_lag={"validator1": c.t + 7})
    F, nobs, span = fs.wait_quiescent(chain.observe, 10.0, 120.0, clock=c, sleep=c.sleep)
    check(c.t >= 1000 + 7 + 10, f"quiescence waits for the application to commit F ({c.t - 1000:.1f}s)")

    # continuous production never becomes quiescent: bounded failure at the deadline
    c = Clock()
    chain = Chain(c, [c.t + i * 1.0 for i in range(500)])
    err = raises(fs.wait_quiescent, chain.observe, 10.0, 60.0, clock=c, sleep=c.sleep)
    check(err is not None and "no quiescent height within 60.0s" in err and c.t - 1000 <= 61.0,
          "block production that never stops fails at the monotonic deadline")

    # a block after quiescence is detected by the confirmation window
    c = Clock()
    chain = Chain(c, [c.t - 20, c.t + 4.2])
    err = raises(fs.confirm_still, chain.observe, 1, 10.0, clock=c, sleep=c.sleep)
    check(err is not None and "block production after quiescence at 1" in err, "a commit during the confirmation window fails")
    c = Clock()
    chain = Chain(c, [c.t - 20])
    check(fs.confirm_still(chain.observe, 1, 10.0, clock=c, sleep=c.sleep) >= 3, "no commit: confirmation passes")

    # --- block time conversion (truncation like the ABCI adapter)
    check(fs.time_ms("2026-11-26T23:48:16.815999999Z") == 1795736896815, "RFC 3339 nanoseconds truncate to ms")
    check(fs.time_ms("2026-11-26T23:48:16Z") == 1795736896000, "RFC 3339 without fraction")
    check(fs.time_ms("2026-11-26T23:48:16.8Z") == 1795736896800, "short fraction is padded")

    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print("final state verification: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
