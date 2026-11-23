// SPDX-License-Identifier: Apache-2.0
//
// Merkle commitments (docs/protocol.md §8).
#pragma once

#include <vector>

#include "yap/common/bytes.hpp"

namespace yap::merkle {

/// A leaf of the sparse Merkle tree: 256-bit path and the hash of the stored value.
struct SmtLeaf {
    Hash32 key{};
    Hash32 value_hash{};
    bool operator==(const SmtLeaf&) const = default;
};

/// smt_key = H_tag("YAP/state-key/v1", raw_key)
[[nodiscard]] Hash32 smt_key(ByteView raw_key);
/// value_hash = H_tag("YAP/state-value/v1", value_bytes)
[[nodiscard]] Hash32 smt_value_hash(ByteView value_bytes);

/// Root of the compact sparse Merkle tree of §8.1: empty subtree = 32 zero bytes; a subtree
/// with exactly one leaf = H_tag("YAP/smt/leaf/v1", key || value_hash) at any depth; otherwise
/// H_tag("YAP/smt/node/v1", left || right). Path bits are taken MSB-first. The result does not
/// depend on the order of `leaves`; duplicate keys are a programming error (std::invalid_argument).
[[nodiscard]] Hash32 smt_root(std::vector<SmtLeaf> leaves);

/// RFC 6962 Merkle Tree Hash with SHA-256 (§8.2).
[[nodiscard]] Hash32 rfc6962_root(const std::vector<Bytes>& items);

}  // namespace yap::merkle
