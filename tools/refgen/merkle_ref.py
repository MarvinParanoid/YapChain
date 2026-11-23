# SPDX-License-Identifier: Apache-2.0
"""Independent reference for docs/protocol.md §8: compact sparse Merkle tree and RFC 6962 root."""
import hashlib

EMPTY = b"\x00" * 32


def tagged_hash(tag, msg):
    t = tag.encode("ascii")
    return hashlib.sha256(bytes([len(t)]) + t + msg).digest()


def smt_key(raw_key):
    return tagged_hash("YAP/state-key/v1", raw_key)


def value_hash(value):
    return tagged_hash("YAP/state-value/v1", value)


def _bit(key, depth):
    return (key[depth // 8] >> (7 - depth % 8)) & 1


def _subtree(leaves, depth):
    # Written as a direct transcription of the definition (no sorting tricks).
    if not leaves:
        return EMPTY
    if len(leaves) == 1:
        k, vh = leaves[0]
        return tagged_hash("YAP/smt/leaf/v1", k + vh)
    left = [leaf for leaf in leaves if _bit(leaf[0], depth) == 0]
    right = [leaf for leaf in leaves if _bit(leaf[0], depth) == 1]
    return tagged_hash("YAP/smt/node/v1", _subtree(left, depth + 1) + _subtree(right, depth + 1))


def smt_root(leaves):
    """leaves: iterable of (key32, value_hash32), keys distinct."""
    leaves = list(leaves)
    assert len({k for k, _ in leaves}) == len(leaves)
    return _subtree(leaves, 0)


def state_root(entries):
    """entries: dict raw_key -> value bytes."""
    return smt_root((smt_key(k), value_hash(v)) for k, v in entries.items())


def rfc6962_root(items):
    if not items:
        return hashlib.sha256(b"").digest()
    if len(items) == 1:
        return hashlib.sha256(b"\x00" + items[0]).digest()
    k = 1
    while k * 2 < len(items):
        k *= 2
    return hashlib.sha256(b"\x01" + rfc6962_root(items[:k]) + rfc6962_root(items[k:])).digest()
