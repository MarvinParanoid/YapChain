// SPDX-License-Identifier: Apache-2.0
#include "yap/merkle/persistent_smt.hpp"

#include <algorithm>

#include "yap/common/hex.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"

namespace yap::merkle {
namespace {

constexpr Byte kLeafTag = 0x00;
constexpr Byte kInternalTag = 0x01;
constexpr std::size_t kMaxDepth = 256;

bool bit_at(const Hash32& key, std::size_t depth) noexcept {
    const unsigned byte = key[depth / 8];
    const unsigned shift = 7U - static_cast<unsigned>(depth % 8);
    return ((byte >> shift) & 1U) != 0;
}

bool is_zero(const Hash32& h) noexcept { return h == Hash32{}; }

}  // namespace

Bytes encode_node(const SmtNode& node) {
    Bytes out;
    out.reserve(65);
    out.push_back(node.leaf ? kLeafTag : kInternalTag);
    out.insert(out.end(), node.a.begin(), node.a.end());
    out.insert(out.end(), node.b.begin(), node.b.end());
    return out;
}

std::optional<SmtNode> decode_node(ByteView bytes) {
    if (bytes.size() != 65 || (bytes[0] != kLeafTag && bytes[0] != kInternalTag)) return std::nullopt;
    SmtNode n;
    n.leaf = bytes[0] == kLeafTag;
    std::copy(bytes.begin() + 1, bytes.begin() + 33, n.a.begin());
    std::copy(bytes.begin() + 33, bytes.end(), n.b.begin());
    return n;
}

Hash32 node_hash(const SmtNode& node) {
    return crypto::TaggedHasher(node.leaf ? crypto::tag::kSmtLeaf : crypto::tag::kSmtNode)
        .update(node.a)
        .update(node.b)
        .finalize();
}

// ------------------------------------------------------------------ updater

std::optional<SmtNode> SmtUpdater::node(const Hash32& hash) const {
    auto it = changes_.find(hash);
    if (it != changes_.end()) return it->second;
    return base_->node(hash);
}

SmtNode SmtUpdater::load(const Hash32& h) const {
    auto n = node(h);
    if (!n) throw SmtCorruption("SMT node missing: " + to_hex(h));
    return *n;
}

Hash32 SmtUpdater::put(const SmtNode& n) {
    const Hash32 h = node_hash(n);
    changes_[h] = n;
    return h;
}

void SmtUpdater::drop(const Hash32& h) {
    // Safe because, in the canonical tree of one version, every stored node occurs exactly once:
    // leaf hashes commit to the full key, and internal nodes never change position.
    if (!is_zero(h)) changes_[h] = std::nullopt;
}

void SmtUpdater::update(const Hash32& key, const std::optional<Hash32>& value_hash) {
    root_ = update_at(root_, 0, key, value_hash);
}

Hash32 SmtUpdater::update_at(const Hash32& h, std::size_t depth, const Hash32& key, const std::optional<Hash32>& vh) {
    // Leaves may sit at depth 256 (keys that differ only in their last bit); nothing deeper exists.
    if (depth > kMaxDepth) throw SmtCorruption("SMT deeper than 256 levels");
    if (is_zero(h)) {
        return vh ? put(SmtNode{true, key, *vh}) : h;
    }
    const SmtNode n = load(h);
    if (n.leaf) {
        if (n.a == key) {
            if (!vh) {
                drop(h);
                return Hash32{};
            }
            if (n.b == *vh) return h;
            drop(h);
            return put(SmtNode{true, key, *vh});
        }
        if (!vh) return h;  // deleting a key that is not present
        const Hash32 fresh = put(SmtNode{true, key, *vh});
        return split(h, n.a, fresh, key, depth);  // the old leaf moves down: it is kept, not dropped
    }
    if (depth == kMaxDepth) throw SmtCorruption("SMT internal node at depth 256");
    const bool bit = bit_at(key, depth);
    const Hash32& child = bit ? n.b : n.a;
    const Hash32& other = bit ? n.a : n.b;
    const Hash32 updated = update_at(child, depth + 1, key, vh);
    if (updated == child) return h;
    drop(h);
    return join(bit, updated, other);
}

Hash32 SmtUpdater::join(bool bit, const Hash32& updated, const Hash32& other) {
    // Keep the compact form: a subtree left with a single leaf collapses into that leaf.
    if (is_zero(updated) && !is_zero(other) && load(other).leaf) return other;
    if (is_zero(other) && (is_zero(updated) || load(updated).leaf)) return updated;
    return put(bit ? SmtNode{false, other, updated} : SmtNode{false, updated, other});
}

Hash32 SmtUpdater::split(const Hash32& old_leaf, const Hash32& old_key, const Hash32& new_leaf,
                         const Hash32& new_key, std::size_t depth) {
    if (depth >= kMaxDepth) throw SmtCorruption("SMT split beyond 256 levels (duplicate key?)");
    const bool ob = bit_at(old_key, depth);
    const bool nb = bit_at(new_key, depth);
    if (ob != nb) {
        return put(ob ? SmtNode{false, new_leaf, old_leaf} : SmtNode{false, old_leaf, new_leaf});
    }
    const Hash32 child = split(old_leaf, old_key, new_leaf, new_key, depth + 1);
    return put(ob ? SmtNode{false, Hash32{}, child} : SmtNode{false, child, Hash32{}});
}

// ------------------------------------------------------------------ verification

namespace {

struct Walker {
    const NodeReader& nodes;
    TreeReport report;

    /// Returns the number of leaves under `h`; `prefix` holds the path bits so far.
    std::size_t walk(const Hash32& h, std::size_t depth, const Hash32& prefix) {
        if (is_zero(h)) return 0;
        if (depth > kMaxDepth) throw SmtCorruption("SMT deeper than 256 levels");
        const auto n = nodes.node(h);
        if (!n) throw SmtCorruption("SMT node missing: " + to_hex(h));
        if (node_hash(*n) != h) throw SmtCorruption("SMT node does not hash to its key: " + to_hex(h));
        if (n->leaf) {
            for (std::size_t d = 0; d < depth; ++d) {
                if (bit_at(n->a, d) != bit_at(prefix, d)) throw SmtCorruption("SMT leaf at a wrong position");
            }
            report.leaves.push_back({n->a, n->b});
            return 1;
        }
        if (depth == kMaxDepth) throw SmtCorruption("SMT internal node at depth 256");
        Hash32 right_prefix = prefix;
        right_prefix[depth / 8] = static_cast<Byte>(right_prefix[depth / 8] | (0x80U >> (depth % 8)));
        const std::size_t l = walk(n->a, depth + 1, prefix);
        const std::size_t r = walk(n->b, depth + 1, right_prefix);
        if (l + r < 2) throw SmtCorruption("SMT internal node covers fewer than two leaves (not compact)");
        ++report.internal_nodes;
        return l + r;
    }
};

}  // namespace

TreeReport verify_tree(const NodeReader& nodes, const Hash32& root) {
    Walker w{nodes, {}};
    w.walk(root, 0, Hash32{});
    return std::move(w.report);  // leaves are produced left to right, i.e. in key order
}

}  // namespace yap::merkle
