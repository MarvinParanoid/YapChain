// SPDX-License-Identifier: Apache-2.0
#include "yap/merkle/merkle.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>

#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"

namespace yap::merkle {
namespace {

bool bit_at(const Hash32& key, std::size_t depth) noexcept {
    const unsigned byte = key[depth / 8];
    const unsigned shift = 7U - static_cast<unsigned>(depth % 8);
    return ((byte >> shift) & 1U) != 0;
}

Hash32 subtree(std::span<const SmtLeaf> leaves, std::size_t depth) {
    if (leaves.empty()) {
        return Hash32{};
    }
    if (leaves.size() == 1) {
        return crypto::TaggedHasher(crypto::tag::kSmtLeaf)
            .update(leaves[0].key)
            .update(leaves[0].value_hash)
            .finalize();
    }
    // Leaves are sorted and share the first `depth` bits; with distinct keys depth stays < 256.
    const auto split = std::partition_point(leaves.begin(), leaves.end(),
                                            [depth](const SmtLeaf& l) { return !bit_at(l.key, depth); });
    const auto mid = static_cast<std::size_t>(split - leaves.begin());
    const Hash32 left = subtree(leaves.first(mid), depth + 1);
    const Hash32 right = subtree(leaves.subspan(mid), depth + 1);
    return crypto::TaggedHasher(crypto::tag::kSmtNode).update(left).update(right).finalize();
}

Hash32 rfc6962(std::span<const Bytes> items) {
    if (items.size() == 1) {
        const Byte prefix = 0x00;
        return crypto::Sha256().update(ByteView(&prefix, 1)).update(items[0]).finalize();
    }
    std::size_t k = 1;
    while (k * 2 < items.size()) k *= 2;  // largest power of two strictly below n
    const Byte prefix = 0x01;
    const Hash32 l = rfc6962(items.first(k));
    const Hash32 r = rfc6962(items.subspan(k));
    return crypto::Sha256().update(ByteView(&prefix, 1)).update(l).update(r).finalize();
}

}  // namespace

Hash32 smt_key(ByteView raw_key) { return crypto::tagged_hash(crypto::tag::kStateKey, raw_key); }
Hash32 smt_value_hash(ByteView value_bytes) { return crypto::tagged_hash(crypto::tag::kStateValue, value_bytes); }

Hash32 smt_root(std::vector<SmtLeaf> leaves) {
    std::sort(leaves.begin(), leaves.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return a.key < b.key; });
    for (std::size_t i = 1; i < leaves.size(); ++i) {
        if (leaves[i].key == leaves[i - 1].key) {
            throw std::invalid_argument("smt_root: duplicate key");
        }
    }
    return subtree(leaves, 0);
}

Hash32 rfc6962_root(const std::vector<Bytes>& items) {
    if (items.empty()) {
        return crypto::sha256({});
    }
    return rfc6962(items);
}

}  // namespace yap::merkle
