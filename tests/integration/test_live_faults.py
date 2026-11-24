#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live fault-tolerance tests of the four-validator network (M2.5).

Every P2P link goes through the netfault proxy (scripts/localnet/netfault.py): faults act on the real
CometBFT P2P connections; RPC and ABCI are never proxied. Every fault and control action is logged with
its time (<dir>/net/faults.log, <dir>/net/events.log). Values are compared strictly by height
(tests/integration/livenet.py).

    test_live_faults.py <scenario> <yap> <yapd> <cometbft> [<yapd-test-hooks>]

Environment: YAP_LIVE_DIR (base directory), YAP_LIVE_KEEP=1 (keep it after success).
"""
import os
import shutil
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from livenet import *  # noqa: E402,F401,F403 - the shared live-test library
from livenet import ln  # noqa: E402


# ------------------------------------------------------------------ fault helpers

def pairs_connected(net, a, b):
    """True if a and b see each other as peers (CometBFT net_info)."""
    ids = {p["node_info"]["id"] for p in a.rpc("net_info")["peers"]}
    return b.meta["node_id"] in ids


def peer_ids(n):
    return {p["node_info"]["id"] for p in n.rpc("net_info")["peers"]}


def link_bytes(net):
    return {k: sum(v["bytes"]) for k, v in net.links().items()}


def wait_peers(net, nodes, want, timeout=90):
    """Each node in `nodes` sees exactly the peers `want(node)` (a set of node ids)."""
    wait_until(lambda: all(peer_ids(n) == want(n) for n in nodes), timeout, "expected P2P peers")


def ids_of(nodes):
    return {n.meta["node_id"] for n in nodes}


def frozen(net, nodes, window, settle=4.0):
    """No node in `nodes` commits a new block during `window` seconds (after `settle` seconds for a block
    already in flight). Heights are sampled every 0.5 s, never concluded from a single RPC answer.
    Returns the frozen heights."""
    time.sleep(settle)
    start = {n.i: n.height() for n in nodes}
    t0 = time.time()
    samples = 0
    while time.time() - t0 < window:
        for n in nodes:
            h = n.height()
            check(h == start[n.i], f"{n} committed height {h} (was {start[n.i]}) without a quorum")
        samples += 1
        time.sleep(0.5)
    note(f"no new block for {window:.0f}s on {nodes} ({samples} samples): heights {start}")
    return start


def check_no_conflicts(net, nodes):
    """Every height committed by any node has one block hash and one state root on every node that has it
    (finalized blocks never conflict)."""
    tips = {n.i: n.height() for n in nodes}
    roots = {n.i: n.roots() for n in nodes}
    top = max(tips.values())
    for h in range(1, top + 1):
        have = [n for n in nodes if tips[n.i] >= h]
        hashes = {n.block(h)["block_id"]["hash"] for n in have}
        check(len(hashes) == 1, f"height {h}: conflicting finalized blocks {hashes}")
        rs = {roots[n.i][h] for n in have if h in roots[n.i]}
        check(len(rs) <= 1, f"height {h}: state roots differ {rs}")
    return top


def alive_pids(net):
    out = []
    for nd in [n.dir for n in net.nodes] + [net.d]:
        for name in ("cometbft", "yapd", "netfault"):
            p = os.path.join(nd, f"{name}.pid")
            if os.path.exists(p):
                pid = int(open(p).read())
                try:
                    os.kill(pid, 0)
                    out.append((nd, name, pid))
                except ProcessLookupError:
                    pass
    return out


# ------------------------------------------------------------------ scenario: infrastructure (M2.5a)

def scenario_infra(net, model, args):
    v = net.validators
    step("A1", "four validators, every P2P link through the fault proxy")
    for n in v:
        n.start()
    net.wait_height(3)
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]})
    links = net.links()
    check(len(links) == 12, f"{len(links)} proxy links")
    for a in v:
        for b in v:
            if a.i < b.i:
                ab, ba = links[f"{a.i}->{b.i}"], links[f"{b.i}->{a.i}"]
                check(ab["open"] + ba["open"] >= 1 and sum(ab["bytes"]) + sum(ba["bytes"]) > 0,
                      f"pair {a.i}-{b.i} has no proxied connection: {ab} {ba}")
    note("each of the 6 validator pairs is connected through its proxy link; peers come only from "
         "persistent_peers (proxy ports), PEX off")

    step("A2", "total isolation: no peers, no blocks, no proxied bytes — there is no other route")
    net.fault("partition", groups=[[0], [1], [2], [3]])
    wait_peers(net, v, lambda n: set(), timeout=30)
    before = link_bytes(net)
    refused0 = sum(x["refused"] for x in net.links().values())
    heights = frozen(net, v, 12)
    after = link_bytes(net)
    check(before == after, "bytes crossed the proxy during total isolation")
    for n in v:
        check(peer_ids(n) == set(), f"{n} has peers during total isolation")
        h, chain = n.query("/chain")   # RPC and the ABCI query connection keep working
        check(h == heights[n.i], f"{n}: /chain at {h}")
        check(n.alive("yapd") and n.alive("cometbft"), f"{n}: a process died")
    refused = sum(x["refused"] for x in net.links().values()) - refused0
    check(refused > 0, "nodes did not even try to redial (expected refused dials at the proxy)")
    note(f"0 peers on every node, 0 bytes forwarded, {refused} redials refused by the proxy; RPC and "
         f"ABCI Query answered on every node")

    step("A3", "heal: connections and block production resume")
    net.fault("heal")
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]}, timeout=60)
    net.wait_height(max(heights.values()) + 3)

    step("A4", "one-way delay 100 ms +- 20 ms jitter on every direction (RTT ~200 ms)")
    net.fault("delay", ms=100, jitter_ms=20)
    h0 = net.min_height()
    net.wait_height(h0 + 4, timeout=120)
    st = net.links()
    delayed = sum(x["delayed"] for x in st.values())
    mean = sum(x["mean_delay_ms"] * x["delayed"] for x in st.values()) / max(delayed, 1)
    check(delayed > 0 and 60 <= mean <= 200, f"delay not applied: {delayed} chunks, mean {mean:.1f} ms")
    net.fault("delay", ms=0, jitter_ms=0)
    note(f"{delayed} chunks held back, mean applied delay {mean:.1f} ms; blocks continued")

    step("A5", "TCP connection resets (not packet loss) and periodic outages; the network recovers")
    net.fault("reset", probability=1.0)
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]}, timeout=60)
    net.fault("flap", period_s=4, down_s=1)
    h0 = net.min_height()
    net.wait_height(h0 + 3, timeout=120)
    net.fault("flap", period_s=0, down_s=0)
    flaps = sum(1 for line in open(os.path.join(net.d, "faults.log")) if '"flap_down"' in line)
    check(flaps >= 1, "no outage happened")
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]}, timeout=60)
    note(f"all connections reset once, {flaps} periodic outage(s); peers and blocks back")

    step("A6", "crash and restart of one component: SIGKILL CometBFT of validator-2, restart it alone")
    n2 = v[2]
    rc = n2.kill("cometbft")
    check(rc == -signal.SIGKILL, f"cometbft exit {rc}")
    net.wait_height(net.min_height([v[0], v[1], v[3]]) + 2, [v[0], v[1], v[3]])
    check(n2.alive("yapd"), "yapd must survive the loss of its CometBFT")
    n2.start_cometbft()
    net.wait_height(net.min_height([v[0], v[1], v[3]]) + 2)

    step("A7", "every node agrees on every height; the fault log has timestamps")
    top = check_no_conflicts(net, v)
    log = [json.loads(line) for line in open(os.path.join(net.d, "faults.log"))]
    check(all("time" in e for e in log) and {e["event"] for e in log} >= {"start", "partition", "heal", "delay",
                                                                          "reset", "flap"}, "fault log")
    note(f"no conflicting block at heights 1..{top}; {len(log)} timestamped fault events")


SCENARIOS = {
    "infra": scenario_infra,
}


def main(scenario, yap, yapd, cometbft, hooks=None):
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="yf-")
    d = os.path.join(base, "net")
    net = None
    ok = False
    try:
        net = Net(d, yap, yapd, cometbft, fault_proxy=True)
        net.hooks = hooks
        model = Model(net)
        SCENARIOS[scenario](net, model, None)

        step("Z", "graceful stop; verify-state of every database; no process left")
        codes = net.stop_all()
        check(all(c.get("cometbft", 0) == 0 and c.get("yapd", 0) == 0 for c in codes.values()), f"exit codes {codes}")
        for n in net.nodes:
            vs = ln.kv(net.cli("verify-state", home=n.meta["yapd_home"]).stdout)
            last_h, last_root = n.commits()[-1]
            check(int(vs["height"]) == last_h and vs["state_root"] == last_root, f"{n}: verify-state {vs}")
            others = [m.roots().get(last_h) for m in net.nodes]
            check(all(r in (None, last_root) for r in others), f"{n}: root of {last_h} differs from other nodes")
            note(f"{n}: verify-state OK at height {vs['height']}, root {vs['state_root'][:16]}…")
        ln.stop_process(d, "netfault")
        check(not alive_pids(net), f"processes left: {alive_pids(net)}")
        check_no_secrets(net)
        ok = True
        print(f"FAULT SCENARIO {scenario} PASSED")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        if net is not None:
            for n in net.nodes:
                for name in ("yapd", "cometbft"):
                    p = os.path.join(n.dir, f"{name}.log")
                    if os.path.exists(p):
                        print(f"--- tail {os.path.basename(n.dir)}/{name}.log", file=sys.stderr)
                        print("\n".join(open(p, errors="replace").read().splitlines()[-12:]), file=sys.stderr)
            print("--- events", file=sys.stderr)
            print("\n".join(net.events[-30:]), file=sys.stderr)
        return 1
    finally:
        if net is not None:
            try:
                net.stop_all()
            except Exception as e:  # noqa: BLE001 - cleanup must not hide the result
                print(f"cleanup: {e}", file=sys.stderr)
        ln.kill_all(d)
        if ok and not os.environ.get("YAP_LIVE_KEEP"):
            shutil.rmtree(base, ignore_errors=True)
        else:
            print(f"artifacts kept in {base}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) not in (5, 6) or sys.argv[1] not in SCENARIOS:
        print(__doc__ + "\nscenarios: " + ", ".join(SCENARIOS), file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], *[os.path.abspath(p) for p in sys.argv[2:]]))
