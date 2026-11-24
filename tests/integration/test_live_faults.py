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


# ------------------------------------------------------------------ scenario: quorum loss and recovery (M2.5b)

def consensus_round(n):
    """(height, round, step) the node's consensus is in right now (RPC consensus_state)."""
    h, r, st = n.rpc("consensus_state")["round_state"]["height/round/step"].split("/")
    return int(h), int(r), int(st)


def send_transfer(net, model, node, sender, to, amount):
    """Builds, registers and broadcasts a Transfer through `node`; returns (hash, CheckTx code)."""
    tx_hex, h = net.make_tx("transfer", sender, nonce_of(node, sender, net), amount, to=to, node=node)
    model.register(tx_hex, "transfer", sender, amount, to=to)
    return h, broadcast_result(node, tx_hex)


def check_all_state(net, model, nodes):
    for n in nodes:
        check_node_state(net, model, n, all_accounts(net))


def scenario_quorum(net, model, args):
    v = net.validators
    v0, v1, v2, v3 = v
    step("Q1-Q2", "four validators (power 10 each, quorum > 26.67 i.e. 3 of 4); stable finalization")
    for n in v:
        n.start()
    net.wait_height(5)

    step("Q3", "several valid transactions")
    sent = [send_transfer(net, model, v[k], s, t, 1_000_000 + k)
            for k, (s, t) in enumerate([("alice", "bob"), ("bob", "carol"), ("carol", "alice")])]
    check(all(c == 0 for _, c in sent), f"CheckTx {sent}")
    for h, _ in sent:
        net.wait_included(h)

    step("Q4-Q5", "stop validator-3: 30 of 40 power remain (> 2/3), blocks continue")
    net.event("stop validator-3")
    check(v3.stop() == {"cometbft": 0, "yapd": 0}, "validator-3 exit codes")
    three = [v0, v1, v2]
    h0 = net.min_height(three)
    net.wait_height(h0 + 4, three, timeout=120)

    step("Q6-Q8", "stop validator-2: 20 of 40 power — no quorum; no block and no emission over a long window")
    net.event("stop validator-2")
    check(v2.stop() == {"cometbft": 0, "yapd": 0}, "validator-2 exit codes")
    two = [v0, v1]
    stall = frozen(net, two, 25)
    hs = max(stall.values())
    chain_before = {n.i: n.query("/chain") for n in two}
    rounds = {n.i: consensus_round(n) for n in two}
    check(all(h == hs + 1 for h, _, _ in rounds.values()), f"consensus should wait at height {hs + 1}: {rounds}")
    # With 20 of 40 power there is never "+2/3 of any" prevote, so timeout_prevote never starts and the
    # round does not advance: consensus waits at height hs+1 (height/round/step from consensus_state).
    note(f"stuck at committed height {hs}; consensus waits at height/round/step "
         f"{[f'{h}/{r}/{st}' for h, r, st in rounds.values()]}")

    step("Q9-Q10", "a transaction accepted into the mempool is not finalized without a quorum")
    pend, code = send_transfer(net, model, v0, "alice", "carol", 7_777_777)
    check(code == 0, f"CheckTx of the pending transaction: {code}")
    wait_until(lambda: all(int(n.rpc("num_unconfirmed_txs")["n_txs"]) >= 1 for n in two), 20,
               "the pending transaction in both mempools")
    frozen(net, two, 10, settle=0)
    check(not net.find_tx(pend, two), "pending transaction finalized without a quorum")
    for n in two:
        h, chain = n.query("/chain")
        check(h == hs and chain == chain_before[n.i][1], f"{n}: state changed without a quorum")
    note(f"pending tx {pend[:16]}… in the mempool of validator-0 and -1, not in any block; "
         f"minted/supply unchanged at {hs}")

    step("Q11-Q13", "restart validator-2 with its data: quorum is back, the pending transaction is executed")
    net.event("restart validator-2")
    v2.start()
    three = [v0, v1, v2]
    net.wait_height(hs + 3, three, timeout=180)
    inc = net.wait_included(pend)
    note(f"consensus resumed at {hs + 1}; pending transaction included at height {inc}")

    step("Q14-Q16", "restart validator-3; all four synchronize; same hashes, roots and state")
    net.event("restart validator-3")
    v3.start()
    tip = net.wait_height(net.min_height(three) + 2, v, timeout=240)
    check_same_chain(net, v, tip - 1)
    check_no_conflicts(net, v)
    check_all_state(net, model, v)
    check_rewards(net, model, v0, 1, tip - 1)
    gaps = [h for h in range(hs + 1, tip) if len(model.signers.get(h, [])) < 3 and h > 1]
    note(f"all four at {tip}; heights 1..{tip - 1} identical; state exact; no block was signed by fewer than "
         f"3 validators {'' if not gaps else gaps}")
    check(not gaps, f"blocks with fewer than 3 COMMIT signatures: {gaps}")


# ------------------------------------------------------------------ scenario: partitions 3+1 and 2+2 (M2.5c)

def cross_bytes(net, groups):
    """Bytes forwarded so far on links between different groups."""
    side = {i: k for k, g in enumerate(groups) for i in g}
    return sum(sum(x["bytes"]) for x in net.links().values()
               if x["src"] in side and x["dst"] in side and side[x["src"]] != side[x["dst"]])


def partition(net, groups):
    """Applies a partition and waits until CometBFT itself reports exactly the in-group peers. Returns
    the evidence: peers per node and the cross-group byte counter."""
    net.fault("partition", groups=groups)
    nodes = {n.i: n for n in net.validators}
    for g in groups:
        members = [nodes[i] for i in g]
        wait_peers(net, members, lambda n, members=members: ids_of(members) - {n.meta["node_id"]}, timeout=60)
    peers = {n.i: sorted(nodes_by_id(net)[p].i for p in peer_ids(n)) for n in net.validators}
    return peers, cross_bytes(net, groups)


def nodes_by_id(net):
    return {n.meta["node_id"]: n for n in net.nodes}


def heal(net):
    net.fault("heal")
    wait_peers(net, net.validators, lambda n: ids_of(net.validators) - {n.meta["node_id"]}, timeout=90)


def catchup_evidence(node, ymark, cmark, lo, hi):
    """How a running node got heights lo..hi after a partition healed: decided blocks (FinalizeBlock
    without ProcessProposal) and whether CometBFT used blocksync or consensus catch-up gossip."""
    ylog, clog = node.log(since=ymark), node.log("cometbft", since=cmark)
    fin = {int(x) for x in re.findall(r"ABCI FinalizeBlock height=(\d+)", ylog)}
    pp = {int(x) for x in re.findall(r"ABCI ProcessProposal height=(\d+)", ylog)}
    missing = set(range(lo, hi + 1))
    check(missing <= fin, f"{node}: heights {sorted(missing - fin)[:5]} not finalized after healing")
    via = "blocksync" if "Time to switch to consensus mode" in clog else "consensus catch-up gossip"
    return len(missing - pp), via


def scenario_partition(net, model, args):
    v = net.validators
    v0, v1, v2, v3 = v
    for n in v:
        n.start()
    net.wait_height(4)
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]})

    step("P1", "partition 3+1: validator-3 isolated, all four processes keep running")
    peers, cross0 = partition(net, [[0, 1, 2], [3]])
    note(f"peers by CometBFT net_info: {peers}")
    three = [v0, v1, v2]
    h_iso = v3.height()
    ymark, cmark = v3.log_size(), v3.log_size("cometbft")

    step("P2", "the connected three keep finalizing; the isolated one finalizes nothing")
    t_maj, c_maj = send_transfer(net, model, v0, "alice", "bob", 2_000_000)
    t_min, c_min = send_transfer(net, model, v3, "bob", "carol", 3_000_000)
    check(c_maj == 0 and c_min == 0, f"CheckTx {c_maj} {c_min}")
    net.wait_height(net.min_height(three) + 6, three, timeout=120)
    frozen(net, [v3], 10, settle=0)
    check(v3.height() == h_iso, f"isolated validator moved from {h_iso} to {v3.height()}")
    wait_until(lambda: len(net.find_tx(t_maj, three)) == 3, 60, "majority transaction on the three")
    check(not net.find_tx(t_min, v), "the isolated side's transaction was finalized during the partition")
    check(cross_bytes(net, [[0, 1, 2], [3]]) == cross0, "bytes crossed the partition")
    top = check_no_conflicts(net, v)
    note(f"isolated validator-3 stayed at {h_iso}; majority reached {net.min_height(three)}; its tx included, "
         f"the isolated side's tx only in validator-3's mempool; 0 bytes across; no conflict up to {top}")

    step("P3", "heal: validator-3 catches up and votes again; its pending transaction is executed")
    tip_before = net.min_height(three)
    heal(net)
    net.wait_height(tip_before + 2, v, timeout=180)
    synced, via = catchup_evidence(v3, ymark, cmark, h_iso + 1, tip_before)
    net.wait_included(t_min, timeout=120)
    model.extend(net.min_height(v), v0)
    wait_until(lambda: model.extend(net.min_height(v), v0) and any(
        model.flags[h].get(v3.address) == COMMIT for h in range(tip_before + 2, net.min_height(v) + 1)),
        120, "validator-3 COMMIT votes after healing")
    note(f"heights {h_iso + 1}..{tip_before} ({tip_before - h_iso} blocks) applied by validator-3 as decided "
         f"blocks ({synced} without ProcessProposal) via {via}; votes COMMIT again")

    step("P4", "partition 2+2: no side has a quorum")
    peers, cross0 = partition(net, [[0, 1], [2, 3]])
    note(f"peers by CometBFT net_info: {peers}")
    m = nonce_of(v0, "alice", net)
    check(m == nonce_of(v2, "alice", net), "both sides agree on alice's nonce before the split")
    a_hex, a = net.make_tx("transfer", "alice", m, 4_000_000, to="bob", node=v0)
    b_hex, b = net.make_tx("transfer", "alice", m, 5_000_000, to="carol", node=v2)
    model.register(a_hex, "transfer", "alice", 4_000_000, to="bob")
    model.register(b_hex, "transfer", "alice", 5_000_000, to="carol")
    ra, rb = broadcast_result(v0, a_hex), broadcast_result(v2, b_hex)
    check(ra == 0 and rb == 0, f"conflicting transactions accepted by each side: {ra} {rb}")
    stall = frozen(net, v, 20)
    check(cross_bytes(net, [[0, 1], [2, 3]]) == cross0, "bytes crossed the 2+2 partition")
    check(not net.find_tx(a, v) and not net.find_tx(b, v), "a conflicting transaction finalized without quorum")
    rounds = {n.i: consensus_round(n)[1] for n in v}
    note(f"same-nonce transfers A (to bob) via validator-0 and B (to carol) via validator-2 accepted by their "
         f"sides; no block for 20 s at {stall}; consensus rounds {rounds}; 0 bytes across")

    step("P5", "heal: consensus continues; at most one of A/B executes; balances, nonce, supply exact")
    heal(net)
    hs = max(stall.values())
    net.wait_height(hs + 3, v, timeout=180)
    wait_until(lambda: nonce_of(v0, "alice", net) == m + 1, 60, "alice nonce m+1")
    tip = net.wait_height(net.min_height(v) + 2, v)
    fa, fb = net.find_tx(a, v), net.find_tx(b, v)
    check(not (fa and fb) and (len(fa) == N or len(fb) == N), f"conflict outcome A={fa} B={fb}")
    wait_until(net.mempools_empty, 60, "the losing transaction to leave every mempool")
    check_same_chain(net, v, tip - 1)
    check_no_conflicts(net, v)
    check_all_state(net, model, v)
    check_rewards(net, model, v0, 1, tip - 1)
    note(f"executed {'A (to bob)' if fa else 'B (to carol)'} only; nonce {m}->{m + 1}; heights 1..{tip - 1} "
         f"identical on all four; state exact")


# ------------------------------------------------------------------ scenario: latency and unstable links (M2.5d)

def percentile(xs, p):
    xs = sorted(xs)
    if not xs:
        return 0.0
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def header_time_ms(hdr):
    """RFC 3339 with nanoseconds -> ms since the epoch."""
    import datetime
    t = hdr["time"]
    main, frac = (t[:-1].split(".") + ["0"])[:2]
    dt = datetime.datetime.strptime(main, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=datetime.timezone.utc)
    return dt.timestamp() * 1000 + int((frac + "000000000")[:9]) / 1e6


def commit_times_ms(node):
    """Wall-clock time (ms) at which this node's yapd made each height durable (its Commit log line)."""
    import datetime
    out = {}
    for t, h in re.findall(r"^(\S+)Z yapd: Commit: height (\d+),", node.log(), re.M):
        dt = datetime.datetime.strptime(t, "%Y-%m-%dT%H:%M:%S.%f").replace(tzinfo=datetime.timezone.utc)
        out[int(h)] = dt.timestamp() * 1000
    return out


def profile_metrics(net, model, lo, hi, incl):
    """Block intervals (wall-clock Commit times on validator-0), extra rounds (LastCommit.round of the
    next block), and flag shares per validator over heights lo..hi."""
    n0 = net.validators[0]
    ct = commit_times_ms(n0)
    times = [ct[h] for h in range(lo, hi + 1)]
    iv = [b - a for a, b in zip(times, times[1:])]
    rounds = [int(n0.block(h + 1)["block"]["last_commit"]["round"]) for h in range(lo, hi)]
    model.extend(hi, n0)
    shares = {}
    for v in net.validators:
        c = {ABSENT: 0, COMMIT: 0, NIL: 0}
        for h in range(lo + 1, hi + 1):
            c[model.flags[h].get(v.address, ABSENT)] += 1
        shares[v.i] = c
    return {"blocks": hi - lo + 1, "interval_median_ms": round(percentile(iv, 0.5)),
            "interval_p95_ms": round(percentile(iv, 0.95)), "extra_rounds": sum(rounds),
            "blocks_with_extra_rounds": sum(1 for r in rounds if r > 0),
            "tx_inclusion_ms": [round(x) for x in incl], "flags": shares}


def run_profile(net, model, name, blocks, txs, timeout):
    """Sends `txs` transfers through rotating validators, then waits for `blocks` blocks under the
    current link profile (finite timeout). Metrics are computed for whatever was produced; inclusion
    time = wall-clock Commit of the including block on validator-0 - wall-clock send time."""
    v = net.validators
    lo = net.min_height(v) + 1
    sends = []
    for k in range(txs):
        node = v[k % len(v)]
        t_ms = time.time() * 1000
        sender = ("alice", "bob", "carol")[k % 3]   # distinct senders: the burst needs no nonce ordering
        h, code = send_transfer(net, model, node, sender, "dave", 10_000 + k)
        check(code == 0, f"{name}: CheckTx {code}")
        sends.append((h, t_ms))
    stalled = False
    try:
        net.wait_height(lo + blocks, v, timeout=timeout)
    except Fail:
        stalled = True
    hi = net.min_height(v) - 1
    incl = []
    for h, t_ms in sends:
        found = net.find_tx(h, v)
        if found:
            height = next(iter(found.values()))[0]
            incl.append(commit_times_ms(v[0])[height] - t_ms)
    if hi - lo < 1:
        return {"stalled": stalled, "blocks": max(0, hi - lo + 1), "tx_inclusion_ms": [round(x) for x in incl]}
    m = profile_metrics(net, model, lo, hi, incl)
    m["stalled"] = stalled
    return m


LATENCY_PROFILES = [
    ("baseline (no fault)", {"ms": 0, "jitter_ms": 0}, None),
    ("one-way 50 ms (RTT 100)", {"ms": 50, "jitter_ms": 0}, None),
    ("one-way 150 ms (RTT 300)", {"ms": 150, "jitter_ms": 0}, None),
    ("one-way 300 ms (RTT 600)", {"ms": 300, "jitter_ms": 0}, None),
    ("one-way 150 +- 120 ms jitter", {"ms": 150, "jitter_ms": 120}, None),
    ("periodic outages 1 s every 10 s", {"ms": 0, "jitter_ms": 0}, {"period_s": 10, "down_s": 1}),
    ("aggressive: outages 2 s every 6 s", {"ms": 0, "jitter_ms": 0}, {"period_s": 6, "down_s": 2}),
    ("aggressive: one-way 1500 ms", {"ms": 1500, "jitter_ms": 0}, None),
]


def scenario_latency(net, model, args):
    v = net.validators
    for n in v:
        n.start()
    net.wait_height(4)
    wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]})
    results = {}
    for name, delay, flap in LATENCY_PROFILES:
        step("L", f"profile: {name} — delay is applied per direction on every P2P link")
        net.fault("delay", **delay)
        if flap:
            net.fault("flap", **flap)
        m = run_profile(net, model, name, blocks=12, txs=3, timeout=180)
        net.fault("delay", ms=0, jitter_ms=0)
        net.fault("flap", period_s=0, down_s=0)
        results[name] = m
        if m["stalled"]:
            note(f"{name}: only {m['blocks']} of 12 blocks within 180 s (stalled); links normalized, "
                 f"checking recovery")
        if "interval_median_ms" in m:
            note(f"{name}: {m['blocks']} blocks; median {m['interval_median_ms']} ms, p95 {m['interval_p95_ms']} ms "
                 f"between blocks; extra rounds {m['extra_rounds']} in {m['blocks_with_extra_rounds']} blocks; "
                 f"tx inclusion {m['tx_inclusion_ms']} ms")
            note("  flags A/C/N per validator: " + "; ".join(
                f"v{i}: {c[ABSENT]}/{c[COMMIT]}/{c[NIL]}" for i, c in m["flags"].items()))
        # Recovery after every profile: links normal, all peers, blocks continue.
        wait_peers(net, v, lambda n: ids_of(v) - {n.meta["node_id"]}, timeout=120)
        net.wait_height(net.min_height(v) + 2, v, timeout=180)
    with open(os.path.join(net.d, "latency-metrics.json"), "w") as f:
        json.dump(results, f, indent=2)
    step("L-end", "no conflicting block, identical roots, exact state on every node")
    tip = net.wait_height(net.min_height(v) + 1, v)
    check_same_chain(net, v, tip - 1)
    check_no_conflicts(net, v)
    check_all_state(net, model, v)
    check_rewards(net, model, v[0], 1, tip - 1)
    print("LATENCY METRICS " + json.dumps(results), flush=True)


SCENARIOS = {
    "infra": scenario_infra,
    "quorum": scenario_quorum,
    "partition": scenario_partition,
    "latency": scenario_latency,
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
