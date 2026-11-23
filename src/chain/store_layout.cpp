// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/store_layout.hpp"

#include "yap/common/hex.hpp"

#include <limits>

namespace yap::chain {

Bytes meta_key() {
    const ByteView k = as_bytes("yapchain/meta");
    return Bytes(k.begin(), k.end());
}

std::optional<StoreMeta> read_meta(const storage::KvStore& store) {
    auto raw = store.get(storage::Table::Meta, meta_key());
    if (!raw) return std::nullopt;
    if (raw->size() < 4) throw StoreCorruption("metadata record is truncated");
    const std::uint32_t version = static_cast<std::uint32_t>((*raw)[0]) | (static_cast<std::uint32_t>((*raw)[1]) << 8) |
                                  (static_cast<std::uint32_t>((*raw)[2]) << 16) |
                                  (static_cast<std::uint32_t>((*raw)[3]) << 24);
    if (version != kSchemaVersion) {
        throw IncompatibleSchema("store schema version " + std::to_string(version) + " is not supported (expected " +
                                 std::to_string(kSchemaVersion) + ")");
    }
    auto meta = ycs::decode<StoreMeta>(*raw);
    if (!meta) throw StoreCorruption("metadata record cannot be decoded");
    return std::move(meta).value();
}

void StoreMeta::encode_to(ycs::Writer& w) const {
    w.u32(schema_version);
    w.string(chain_id);
    w.fixed(genesis_hash);
    w.u64(last_height);
    w.fixed(last_state_root);
}

StoreMeta StoreMeta::decode_from(ycs::Reader& r) {
    StoreMeta m;
    m.schema_version = r.u32();
    if (r.ok() && m.schema_version != kSchemaVersion) {
        // Refuse to interpret anything else: the layout of later fields may differ.
        r.fail(ycs::DecodeError::InvalidContent);
        return m;
    }
    m.chain_id = r.string(32);
    m.genesis_hash = r.fixed<32>();
    m.last_height = r.u64();
    m.last_state_root = r.fixed<32>();
    return m;
}

void BlockRecord::encode_to(ycs::Writer& w) const {
    w.u64(height);
    w.fixed(state_root);
    w.length(codes.size());
    for (const auto c : codes) w.u32(c);
}

BlockRecord BlockRecord::decode_from(ycs::Reader& r) {
    BlockRecord b;
    b.height = r.u64();
    b.state_root = r.fixed<32>();
    // No artificial cap: the record was written by this node for a block it executed, and the
    // count is still bounded by the bytes present. (A 100 000 cap made a store unreadable after a
    // legitimately committed block with more transactions — audit M1.5.)
    const auto n = r.vec_length(std::numeric_limits<std::uint32_t>::max(), 4);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) b.codes.push_back(r.u32());
    return b;
}

Bytes results_key(std::uint64_t height) {
    Bytes k(8);
    for (int i = 7; i >= 0; --i) {
        k[static_cast<std::size_t>(i)] = static_cast<Byte>(height);
        height >>= 8;
    }
    return k;
}

std::optional<merkle::SmtNode> StoreNodeReader::node(const Hash32& hash) const {
    auto raw = store_->get(storage::Table::Smt, hash);
    if (!raw) return std::nullopt;
    auto n = merkle::decode_node(*raw);
    if (!n) throw merkle::SmtCorruption("undecodable SMT node " + to_hex(hash));
    return n;
}

Hash32 apply_state_changes(merkle::SmtUpdater& smt, const state::Overlay& block_state) {
    for (const auto& [k, v] : block_state.changes()) {
        smt.update(merkle::smt_key(k), v ? std::optional<Hash32>(merkle::smt_value_hash(*v)) : std::nullopt);
    }
    return smt.root();
}

}  // namespace yap::chain
