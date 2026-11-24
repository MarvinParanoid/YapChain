#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live test: four independent validators — real CometBFT v0.40.0 + yapd + the real `yap` CLI (M2.4).

Every validator is a separate CometBFT process and a separate yapd with its own LMDB, socket, ports,
validator key and node key; they talk only through CometBFT P2P on loopback. Everything goes through
processes, RPC and logs; no library code is called.

Values are compared strictly at the same height: block hashes and headers by height, every node's
state root of height H from its own Commit log, execution results by height. Account queries answer at
the node's latest committed height (historical queries are not supported); every answer is checked
against an independent model of the state AT THAT HEIGHT, built here from consensus data (blocks and
their last commits) and the protocol rules (protocol.md §9.3, §9.5; economics.md §8).

    test_live_multival.py <yap> <yapd> <cometbft>

Environment: YAP_LIVE_DIR (base directory), YAP_LIVE_KEEP=1 (keep the directory after success).
On failure the directory, every node's logs and a tail of each log are kept/printed.
"""
import hashlib
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from livenet import *  # noqa: E402,F401,F403 - the shared live-test library
from livenet import ln  # noqa: E402


# ------------------------------------------------------------------ scenario

def phase_consensus(net, model):
    step("B1", f"start {N} validators: separate CometBFT + yapd + LMDB + socket + ports + keys each")
    gen = {hashlib.sha256(open(os.path.join(n.meta["cometbft_home"], "config", "genesis.json"), "rb").read())
           .hexdigest() for n in net.nodes}
    check(gen == {net.info["genesis_sha256"]}, f"genesis.json differs between nodes: {gen}")
    check(len({n.meta["socket"] for n in net.nodes}) == N and len({n.meta["yapd_home"] for n in net.nodes}) == N,
          "sockets or databases are shared")
    for n in net.nodes:
        n.start()

    step("B2", "every node passes InitChain with the same genesis and the same app_hash")
    wait_until(lambda: all("InitChain: genesis" in n.log() for n in net.nodes), 60, "InitChain on every node")
    inits = set()
    for n in net.nodes:
        m = re.search(r"InitChain: genesis ([0-9a-f]{64}), app_hash ([0-9a-f]{64})", n.log())
        check(m.group(1) == net.info["genesis_hash"], f"{n}: genesis hash {m.group(1)}")
        req = re.search(r"ABCI InitChain chain_id=(\S+) initial_height=(\d+) validators=(\d+) app_version=(\d+) "
                        r"vote_ext_height=(\d+)", n.log())
        check(req and req.groups() == (net.info["chain_id"], "1", str(N), "1", "0"), f"{n}: InitChain {req}")
        inits.add(m.group(2))
    check(len(inits) == 1, f"InitChain app_hash differs: {inits}")
    root0 = inits.pop()
    note(f"genesis {net.info['genesis_hash'][:16]}…, InitChain app_hash {root0[:16]}… on all {N}")

    step("B3", "P2P: every node is connected to the other three")
    wait_until(lambda: all(n.peers() == N - 1 for n in net.nodes), 60, "full P2P mesh")

    step("B4", "at least 30 blocks")
    tip = net.wait_height(31, timeout=180)
    check(net.nodes[0].header(1)["app_hash"].lower() == root0, "header(1).app_hash != InitChain app_hash")

    step("B5", "every validator proposed at least once")
    proposers = {}
    for h in range(1, 31):
        proposers.setdefault(net.nodes[0].header(h)["proposer_address"], []).append(h)
    check(set(proposers) == set(net.by_address), f"proposers of 1..30: {sorted(proposers)}")
    note("proposed heights: " + "; ".join(f"{net.by_address[a]}: {len(hs)}" for a, hs in sorted(
        proposers.items(), key=lambda x: net.by_address[x[0]].i)))

    step("B6", "ProcessProposal runs on validators that are not the proposer, and accepts")
    for n in net.nodes:
        pp = process_proposals(n)
        foreign = [(h, p) for h, p, r in pp if p != n.address]
        check(foreign, f"{n}: no ProcessProposal of a foreign proposal")
        check(all(r == "ACCEPT" for _, _, r in pp), f"{n}: a proposal was rejected in a normal run")
        for h, p in foreign[:3]:
            check(net.nodes[0].header(h)["proposer_address"] == p, f"{n}: ProcessProposal proposer of {h}")
        note(f"{n}: {len(pp)} ProcessProposal, {len(foreign)} of them for other proposers")

    step("B7-B8", "same block hash, app_hash and results on every node; Header(H+1).AppHash = root(H)")
    check_same_chain(net, net.nodes, tip - 1)
    roots = {n.i: n.roots() for n in net.nodes}
    for h in range(1, tip):
        check(len({roots[i][h] for i in roots}) == 1, f"state root of {h} differs between nodes")
    note(f"heights 1..{tip - 1} identical on all {N} nodes; root({tip - 1}) = {roots[0][tip - 1][:16]}…")

    step("B9-B10", "identical execution, supply invariants on every node (model at each answer's height)")
    for n in net.nodes:
        h = check_node_state(net, model, n, all_accounts(net))
        note(f"{n}: state exact at height {h}")
    return tip





def phase_transactions(net, model):
    n0, n1, n2, n3 = net.nodes
    marks = {n.i: n.log_size() for n in net.nodes}
    YAP = YIP

    step("C1-C4", "Transfer via validator 0, Transfer via 1, Burn via 2, Transfer via 3 (different senders)")
    plan = [(n0, "transfer", "alice", "bob", 1_500_000), (n1, "transfer", "bob", "carol", 2 * YAP),
            (n2, "burn", "carol", None, 3 * YAP), (n3, "transfer", "dave", "alice", 1 * YAP)]
    sent = []
    for node, kind, sender, to, amount in plan:
        tx_hex, h = net.make_tx(kind, sender, nonce_of(node, sender, net), amount, to=to, node=node)
        model.register(tx_hex, kind, sender, amount, to=to)
        check(broadcast_result(node, tx_hex) == 0, f"{node} rejected {kind} of {sender}")
        sent.append((node, kind, sender, h))
    for node, kind, sender, h in sent:
        height = net.wait_included(h)
        seen = sorted(n.i for n in net.nodes if any(t == "NEW" and x == h for t, x, _, _ in checktx_log(n, marks[n.i])))
        check(node.i in seen and len(seen) >= 2, f"{kind} of {sender} via {node}: CheckTx only on {seen}")
        proposer = net.by_address[n0.header(height)["proposer_address"]]
        note(f"{kind} {sender} via {node}: height {height} (proposer {proposer}); "
             f"reached by P2P and checked on validators {seen}")

    step("C5", "five sequential nonces of one sender in one burst through validator 1")
    base = nonce_of(n1, "alice", net)
    burst = []
    for k in range(5):
        tx_hex, h = net.make_tx("transfer", "alice", base + k, 10_000 + k, to="bob", node=n1)
        model.register(tx_hex, "transfer", "alice", 10_000 + k, to="bob")
        burst.append((tx_hex, h))
    for tx_hex, _ in burst:
        check(broadcast_result(n1, tx_hex) == 0, "sequential nonce rejected by CheckTx")
    heights = [net.wait_included(h) for _, h in burst]
    check(heights == sorted(heights), f"nonces executed out of order: {heights}")
    note(f"nonces {base}..{base + 4} included at heights {heights}")

    step("C6", "the same bytes sent to three nodes are executed once; a later resend is refused")
    tx_hex, h = net.make_tx("transfer", "bob", nonce_of(n2, "bob", net), 500_000, to="alice", node=n2)
    model.register(tx_hex, "transfer", "bob", 500_000, to="alice")
    answers = {n.i: broadcast_result(n, tx_hex) for n in (n2, n0, n3)}
    check(answers[2] == 0 and all(a in (0, "cache") for a in answers.values()), f"same bytes: {answers}")
    height = net.wait_included(h)
    late = broadcast_result(n1, tx_hex)
    check(late in ("cache", ERR_NONCE), f"resend after inclusion: {late}")
    note(f"answers {answers}, included once at height {height}, resend to validator-1 -> {late}")

    step("C7", "conflicting transactions with the same nonce through validators 0 and 3")
    m = nonce_of(n0, "alice", net)
    a_hex, a = net.make_tx("transfer", "alice", m, 2 * YAP, to="bob", node=n0)
    b_hex, b = net.make_tx("transfer", "alice", m, 3 * YAP, to="carol", node=n3)
    model.register(a_hex, "transfer", "alice", 2 * YAP, to="bob")
    model.register(b_hex, "transfer", "alice", 3 * YAP, to="carol")
    ra, rb = parallel(lambda: broadcast_result(n0, a_hex), lambda: broadcast_result(n3, b_hex))
    check(ra in (0, ERR_NONCE) and rb in (0, ERR_NONCE) and 0 in (ra, rb), f"conflict CheckTx: {ra} {rb}")
    wait_until(lambda: nonce_of(n0, "alice", net) == m + 1 and nonce_of(n3, "alice", net) == m + 1, 60,
               "one of the conflicting transactions")
    net.wait_height(net.min_height() + 2)
    fa, fb = net.find_tx(a), net.find_tx(b)
    check((len(fa) == N) != (len(fb) == N) and not (fa and fb), f"conflict outcome A={fa} B={fb}")
    wait_until(net.mempools_empty, 30, "the losing transaction to leave every mempool")
    losers = {(b if fa else a): next(iter((fa or fb).values()))[0]}  # loser -> height of the winner
    note(f"CheckTx answers {ra}/{rb}; executed {'A (to bob)' if fa else 'B (to carol)'} only, nonce {m}->{m + 1}")

    step("C8-C9", "insufficient balance and TTL are refused by CheckTx on any node")
    poor_hex, _ = net.make_tx("transfer", "carol", nonce_of(n2, "carol", net), 10_000 * YAP, to="bob", node=n2)
    check(broadcast_result(n2, poor_hex) == ERR_INSUFFICIENT_FUNDS, "insufficient balance")
    tip = n3.height()
    old_hex, _ = net.make_tx("transfer", "alice", nonce_of(n3, "alice", net), 1, to="bob", expires=tip - 1, node=n3)
    check(broadcast_result(n3, old_hex) == ERR_EXPIRED, "expired transaction")
    far_hex, _ = net.make_tx("transfer", "alice", nonce_of(n3, "alice", net), 1, to="bob", expires=tip + 5_000, node=n3)
    check(broadcast_result(n3, far_hex) == ERR_TTL_TOO_LONG, "TTL above max_tx_ttl_blocks")

    step("C10", "competing spends of dave's whole balance through validators 1 and 2")
    k = nonce_of(n1, "dave", net)
    bal = int(n1.query("/account/" + net.addresses["dave"])[1]["balance"])
    d1_hex, d1 = net.make_tx("transfer", "dave", k, bal - FEE, to="bob", node=n1)
    d3_hex, d3 = net.make_tx("transfer", "dave", k, bal - FEE, to="carol", node=n2)
    d2_hex, _ = net.make_tx("transfer", "dave", k + 1, bal - FEE, to="alice", node=n1)
    model.register(d1_hex, "transfer", "dave", bal - FEE, to="bob")
    model.register(d3_hex, "transfer", "dave", bal - FEE, to="carol")
    r1, r3 = parallel(lambda: broadcast_result(n1, d1_hex), lambda: broadcast_result(n2, d3_hex))
    r2 = broadcast_result(n1, d2_hex)
    check(0 in (r1, r3) and r2 in (ERR_INSUFFICIENT_FUNDS, ERR_NONCE), f"competing spends: {r1} {r3} then {r2}")
    wait_until(lambda: nonce_of(n1, "dave", net) == k + 1, 60, "one competing spend")
    net.wait_height(net.min_height() + 2)
    f1, f3 = net.find_tx(d1), net.find_tx(d3)
    check((len(f1) == N) != (len(f3) == N) and not (f1 and f3), f"competing spends outcome {f1} {f3}")
    losers[d3 if f1 else d1] = next(iter((f1 or f3).values()))[0]
    for n in net.nodes:
        acc = n.query("/account/" + net.addresses["dave"])[1]
        check(acc["balance"] == "0", f"{n}: dave balance {acc['balance']} after spending everything once")
    wait_until(net.mempools_empty, 30, "empty mempools")
    note(f"answers {r1}/{r3}, second nonce {r2}; one spend executed, dave balance 0 on all nodes")

    step("C11", "non-empty mempool at Commit: real CheckTx RECHECK on live nodes")
    rechecks = {}
    burst_hashes = []
    for attempt in range(4):
        base = nonce_of(n0, "alice", net)
        for j in range(30):
            tx_hex, h = net.make_tx("transfer", "alice", base + j, 1_000 + j, to="carol", node=n0)
            model.register(tx_hex, "transfer", "alice", 1_000 + j, to="carol")
            check(broadcast_result(n0, tx_hex) == 0, f"burst tx {j} rejected")
            burst_hashes.append(h)
        for h in burst_hashes[-30:]:
            net.wait_included(h)
        rechecks = {n.i: [(x, c, ch) for t, x, c, ch in checktx_log(n, marks[n.i]) if t == "RECHECK"]
                    for n in net.nodes}
        if sum(map(len, rechecks.values())) > 0:
            break
    total = sum(map(len, rechecks.values()))
    check(total > 0, "no CheckTx RECHECK observed after 4 bursts of 30 transactions")
    for n in net.nodes:
        codes = {}
        for _, c, _ in rechecks[n.i]:
            codes[c] = codes.get(c, 0) + 1
        note(f"{n}: {len(rechecks[n.i])} RECHECK, result codes {codes}")
    # Every RECHECK in this phase concerns a transaction we built. A valid one stays valid. A conflict
    # loser is still valid after Commits below the winner's height and must fail with ERR_NONCE after
    # the Commit that executed the winner (then CometBFT drops it).
    evicted = set()
    for n in net.nodes:
        for x, c, committed in rechecks[n.i]:
            check(x in model.txs, f"{n}: RECHECK of an unknown transaction {x}")
            if x in losers:
                want = ERR_NONCE if committed >= losers[x] else 0
                check(c == want, f"{n}: RECHECK of a conflict loser after Commit {committed} (winner at "
                                 f"{losers[x]}) returned {c}, expected {want}")
                if c:
                    evicted.add(n.i)
            else:
                check(c == 0, f"{n}: a valid transaction failed RECHECK with {c} ({x[:16]}…)")
    note(f"RECHECK evicted losing conflicting transactions (ERR_NONCE) on validators {sorted(evicted)}"
         if evicted else "no conflict loser was left in a mempool at Commit this time (only valid RECHECKs)")

    step("C12", "no stuck transactions, no double execution, identical results everywhere")
    wait_until(net.mempools_empty, 30, "empty mempools after the burst")
    tip = net.wait_height(net.min_height() + 1)
    check_same_chain(net, net.nodes, tip - 1)
    for n in net.nodes:
        check_node_state(net, model, n, all_accounts(net))
    model.extend(tip - 1, n0)
    expected = [h for h in model.txs if h in model.included]
    for h in expected:
        found = net.find_tx(h)
        check(len(found) == N and len(set(found.values())) == 1, f"tx {h[:16]}…: {found}")
    note(f"{len(model.included)} transactions executed exactly once; all {N} nodes agree up to height {tip - 1}")
    return marks



def phase_rewards(net, model):
    n0, n3 = net.nodes[0], net.nodes[3]
    step("D1", "scenario A, all four running: decided_last_commit, powers, rewards by actual COMMIT votes")
    tip = net.wait_height(net.min_height() + 1)
    for n in net.nodes:
        counts = check_rewards(net, model, n, 1, tip - 1)
    full = sum(1 for h in range(2, tip) if len(model.signers[h]) == N)
    note(f"heights 1..{tip - 1}: flags {counts} (ABSENT {counts[ABSENT]}, NIL {counts[NIL]}); "
         f"{full} heights signed by all {N}; height 1: no votes, validators' share {V_SHARE} to treasury")
    note(f"per signer: {V_SHARE * POWER // (POWER * N)} yip with 4 signers, "
         f"{V_SHARE * POWER // (POWER * 3)} with 3; pool {POOL_SHARE}, treasury {T_SHARE} + remainder")

    step("D2", "scenario B: stop CometBFT and yapd of validator-3; the other three keep producing blocks")
    codes = n3.stop()
    check(codes == {"cometbft": 0, "yapd": 0}, f"validator-3 exit codes {codes}")
    stopped = int(re.findall(r"stopped at height (\d+)", n3.log())[-1])
    live = net.up()
    start = net.min_height(live)
    net.wait_height(start + 5, live, timeout=120)
    note(f"validator-3 stopped at height {stopped}; the other three went from {start} to {net.min_height(live)}")

    step("D3", "ABSENT observed for validator-3; it gets nothing for a missing vote")
    def absent_heights():
        tip = net.min_height(live)
        model.extend(tip, live[0])
        return [h for h in range(stopped + 1, tip + 1) if model.flags[h].get(n3.address) == ABSENT]
    absent = wait_until(lambda: (lambda a: a if len(a) >= 3 else None)(absent_heights()), 120,
                        "three blocks with validator-3 ABSENT")
    tip = net.wait_height(net.min_height(live) + 1, live)
    for n in live:
        check_rewards(net, model, n, stopped, tip - 1)
        check_node_state(net, model, n, all_accounts(net))
    for h in absent:
        check(n3.reward not in model.rewards[h], f"validator-3 rewarded at height {h} without a vote")
    three = [h for h in absent if len(model.signers[h]) == 3]
    check(three, "no height with exactly the three live signers")
    h = three[0]
    note(f"ABSENT at heights {absent[:8]}{'…' if len(absent) > 8 else ''}; e.g. height {h}: signers "
         f"{[str(net.by_address[a]) for a in model.signers[h]]} get {model.rewards[h][live[0].reward]} each, "
         f"validator-3 0, treasury {T_SHARE} + {V_SHARE - 3 * (V_SHARE // 3)}")
    nils = [(hh, a) for hh in sorted(model.flags) for a, f in model.flags[hh].items() if f == NIL]
    for hh, a in nils:  # observed live: the validator precommitted nil; it must not be paid for that height
        check(model.reward_of[a] not in model.rewards[hh], f"NIL validator {net.by_address[a]} rewarded at {hh}")
    note("NIL observed live: " + (", ".join(f"height {hh} {net.by_address[a]} (not rewarded; signers "
                                            f"{len(model.signers[hh])})" for hh, a in nils) if nils else
                                  "none in this run (NIL is covered by the adapter test with an exact message)"))
    return stopped


def phase_blocksync(net, model, stopped):
    n3 = net.nodes[3]
    live = net.up()
    step("E1-E2", f"validator-3 stays down while the others produce at least 20 more blocks")
    net.wait_height(stopped + 22, live, timeout=240)
    behind = net.min_height(live)
    note(f"validator-3 at {stopped}, network at {behind}")

    step("E3-E5", "restart validator-3 with its own data; it catches up through CometBFT blocksync")
    cmark, ymark = n3.log_size("cometbft"), n3.log_size("yapd")
    n3.start()
    wait_until(lambda: n3.height() >= behind and not n3.rpc("status")["sync_info"]["catching_up"], 180,
               "validator-3 to catch up")
    clog = n3.log("cometbft", since=cmark)
    m = re.search(r"ABCI Handshake App Info\s+module=consensus height=(\d+)", clog)
    check(m and int(m.group(1)) == stopped, f"handshake height {m and m.group(1)} != stopped height {stopped}")
    sw = re.search(r"Time to switch to consensus mode!\s+module=blocksync height=(\d+)", clog)
    check(sw, "validator-3 never switched from blocksync to consensus")
    switch = int(sw.group(1))
    synced = switch - (stopped + 1)
    check(synced >= 20, f"only {synced} blocks came through blocksync (switch at {switch}, stopped at {stopped})")
    ylog = n3.log(since=ymark)
    fin = {int(x) for x in re.findall(r"ABCI FinalizeBlock height=(\d+)", ylog)}
    pp = {int(x) for x in re.findall(r"ABCI ProcessProposal height=(\d+)", ylog)}
    bs = set(range(stopped + 1, switch))
    check(bs <= fin, f"blocksync heights not finalized by validator-3's yapd: {sorted(bs - fin)[:5]}")
    check(not (bs & pp), "ProcessProposal ran for blocksync heights (they must arrive as decided blocks)")
    note(f"handshake at {stopped}; heights {stopped + 1}..{switch - 1} ({synced} blocks) applied via blocksync "
         f"(FinalizeBlock without ProcessProposal); consensus from {switch}")

    step("E6-E7", "same block hash and app_hash at every common height; balances, nonces, supply")
    tip = net.wait_height(net.min_height(net.nodes) + 1, net.nodes)
    check_same_chain(net, net.nodes, tip - 1)
    roots = {n.i: n.roots() for n in net.nodes}
    for h in range(1, tip):
        check(len({roots[i][h] for i in roots}) == 1, f"state root of {h} differs between nodes")
    heights3 = sorted(roots[3])
    check(heights3 == list(range(1, len(heights3) + 1)), "validator-3 committed a height twice or skipped one")
    for n in net.nodes:
        check_node_state(net, model, n, all_accounts(net))

    step("E8", "validator-3 takes part in consensus again: COMMIT votes, ProcessProposal, its own proposal")
    def participating():
        tip = net.min_height(net.nodes)
        model.extend(tip, net.nodes[0])
        votes = [h for h in range(switch + 1, tip + 1) if model.flags[h].get(n3.address) == COMMIT]
        proposed = [h for h in range(switch, tip + 1)
                    if net.nodes[0].header(h)["proposer_address"] == n3.address]
        return (votes, proposed) if votes and proposed else None
    votes, proposed = wait_until(participating, 120, "validator-3 voting and proposing again")
    foreign = [p for h, p, r in process_proposals(n3, ymark) if h >= switch and p != n3.address]
    check(foreign, "validator-3 did not process other validators' proposals after catching up")
    note(f"COMMIT votes again from height {votes[0]}, proposed height {proposed[0]}, "
         f"{len(foreign)} foreign proposals processed")

    step("E9", "no double emission: minted = genesis + 2 YAP x height on every node")
    tip = net.wait_height(net.min_height(net.nodes) + 1, net.nodes)
    for n in net.nodes:
        check_rewards(net, model, n, stopped, tip - 1)
        h = check_node_state(net, model, n, all_accounts(net))
        note(f"{n}: exact at height {h}")


def main(yap, yapd, cometbft):
    base = os.environ.get("YAP_LIVE_DIR") or tempfile.mkdtemp(prefix="yn-")
    d = os.path.join(base, "net")
    net = None
    ok = False
    try:
        net = Net(d, yap, yapd, cometbft)
        model = Model(net)
        phase_consensus(net, model)
        phase_transactions(net, model)
        stopped = phase_rewards(net, model)
        phase_blocksync(net, model, stopped)

        step("Z", "graceful stop of all nodes; verify-state of all four databases")
        codes = net.stop_all()
        check(all(c == {"cometbft": 0, "yapd": 0} for c in codes.values()), f"exit codes {codes}")
        for n in net.nodes:
            vs = ln.kv(net.cli("verify-state", home=n.meta["yapd_home"]).stdout)
            last_h, last_root = n.commits()[-1]
            check(int(vs["height"]) == last_h and vs["state_root"] == last_root, f"{n}: verify-state {vs}")
            others = [m.roots().get(last_h) for m in net.nodes]
            check(all(r in (None, last_root) for r in others), f"{n}: root of {last_h} differs from other nodes")
            note(f"{n}: verify-state OK at height {vs['height']}, root {vs['state_root'][:16]}…")
        size = check_no_secrets(net)
        note(f"no password or private key in {size} bytes of node logs (traces enabled)")
        ok = True
        print("FOUR-VALIDATOR LIVE E2E PASSED")
        return 0
    except (Fail, ln.LocalnetError) as e:
        print(f"FAILED: {e}", file=sys.stderr)
        for i in range(N):
            for name in ("yapd", "cometbft"):
                p = os.path.join(d, f"node{i}", f"{name}.log")
                if os.path.exists(p):
                    print(f"--- tail node{i}/{name}.log", file=sys.stderr)
                    print("\n".join(open(p, errors="replace").read().splitlines()[-15:]), file=sys.stderr)
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
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*[os.path.abspath(p) for p in sys.argv[1:]]))
