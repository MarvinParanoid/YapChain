# SPDX-License-Identifier: Apache-2.0
"""Independent reference for block emission (docs/protocol.md §9.5, ADR-0015, ADR-0021)."""
import hashlib

COMMIT = 2


def tagged_hash(tag, msg):
    t = tag.encode("ascii")
    return hashlib.sha256(bytes([len(t)]) + t + msg).digest()


TREASURY = tagged_hash("YAP/sys-account/v1", b"treasury")
REWARDS_POOL = tagged_hash("YAP/sys-account/v1", b"rewards-pool")


def emission(params, total_minted, last_commit, validators):
    """last_commit: list of (address(bytes20), power, flag); validators: addr -> (power, reward_account).
    Returns (credits: dict account -> amount, only non-zero; minted)."""
    reward = min(params["block_reward"], params["max_issuance"] - total_minted)
    if reward == 0:
        return {}, 0
    v = reward * params["reward_validators_bps"] // 10000
    p = reward * params["reward_pool_bps"] // 10000
    t = reward - v - p
    credits = {}

    def credit(acct, amount):
        if amount:  # ADR-0021: zero credits are no-ops
            credits[acct] = credits.get(acct, 0) + amount

    signers = sorted(addr for addr, _, flag in last_commit if flag == COMMIT)
    if not signers:
        t += v
    else:
        w = sum(validators[a][0] for a in signers)
        distributed = 0
        for a in signers:
            share = v * validators[a][0] // w
            credit(validators[a][1], share)
            distributed += share
        t += v - distributed
    credit(REWARDS_POOL, p)
    credit(TREASURY, t)
    return credits, reward
