// SPDX-License-Identifier: Apache-2.0
//
// Persistent, incrementally updated sparse Merkle tree (ADR-0023). Produces exactly the root of
// protocol.md §8.1 (same as smt_root()), but each key update touches only the nodes on its path.
//
// Nodes are content-addressed by their hash:
//   leaf     = 0x00 || key(32) || value_hash(32)   hash = H_tag("YAP/smt/leaf/v1", key || value_hash)
//   internal = 0x01 || left(32)  || right(32)       hash = H_tag("YAP/smt/node/v1", left || right)
// The empty subtree is the all-zero hash and is never stored. The stored tree is always in the
// canonical compact form of §8.1: every subtree with exactly one leaf is that leaf node, so every
// internal node covers at least two leaves.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "yap/common/bytes.hpp"
#include "yap/merkle/merkle.hpp"

namespace yap::merkle {

struct SmtNode {
    bool leaf = true;
    Hash32 a{};  ///< leaf: key;        internal: left child hash
    Hash32 b{};  ///< leaf: value_hash; internal: right child hash
    bool operator==(const SmtNode&) const = default;
};

[[nodiscard]] Bytes encode_node(const SmtNode& node);
[[nodiscard]] std::optional<SmtNode> decode_node(ByteView bytes);
[[nodiscard]] Hash32 node_hash(const SmtNode& node);

/// Read access to stored nodes. Implementations throw if the backing storage fails.
class NodeReader {
public:
    virtual ~NodeReader() = default;
    [[nodiscard]] virtual std::optional<SmtNode> node(const Hash32& hash) const = 0;
};

/// Error raised when the tree references a node that is missing or malformed (corruption).
class SmtCorruption : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Applies updates on top of a stored tree without writing to it; the resulting node insertions
/// and deletions are exposed for one atomic store write.
class SmtUpdater final : public NodeReader {
public:
    SmtUpdater(const NodeReader& base, const Hash32& root) : base_(&base), root_(root) {}

    /// Sets (value_hash present) or removes (nullopt) the leaf for `key`.
    void update(const Hash32& key, const std::optional<Hash32>& value_hash);
    [[nodiscard]] const Hash32& root() const noexcept { return root_; }
    /// Node writes since construction: hash -> node, or nullopt for a deletion.
    [[nodiscard]] const std::map<Hash32, std::optional<SmtNode>>& changes() const noexcept { return changes_; }

    [[nodiscard]] std::optional<SmtNode> node(const Hash32& hash) const override;

private:
    Hash32 put(const SmtNode& n);
    void drop(const Hash32& h);
    SmtNode load(const Hash32& h) const;
    Hash32 update_at(const Hash32& h, std::size_t depth, const Hash32& key, const std::optional<Hash32>& vh);
    Hash32 join(bool bit, const Hash32& updated, const Hash32& other);
    Hash32 split(const Hash32& old_leaf, const Hash32& old_key, const Hash32& new_leaf, const Hash32& new_key,
                 std::size_t depth);

    const NodeReader* base_;
    Hash32 root_;
    std::map<Hash32, std::optional<SmtNode>> changes_;
};

struct TreeReport {
    std::vector<SmtLeaf> leaves;     ///< every leaf reachable from the root, in key order
    std::size_t internal_nodes = 0;  ///< reachable internal nodes
};

/// Full verification of a stored tree: every reachable node exists, decodes, hashes to its key,
/// sits at a position consistent with its key bits, and the tree is in canonical compact form.
/// Throws SmtCorruption describing the first problem.
[[nodiscard]] TreeReport verify_tree(const NodeReader& nodes, const Hash32& root);

}  // namespace yap::merkle
