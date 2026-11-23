// SPDX-License-Identifier: Apache-2.0
//
// Node-local storage layout (ADR-0022). Not part of consensus.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "yap/codec/ycs.hpp"
#include "yap/merkle/persistent_smt.hpp"
#include "yap/state/kv.hpp"
#include "yap/storage/store.hpp"

namespace yap::chain {

inline constexpr std::uint32_t kSchemaVersion = 1;

/// The store is damaged or inconsistent. Never repaired or recreated automatically.
class StoreCorruption : public storage::StorageError {
public:
    using storage::StorageError::StorageError;
};

/// The store was written with a schema version this node does not support.
class IncompatibleSchema : public storage::StorageError {
public:
    using storage::StorageError::StorageError;
};

/// Reads and decodes the metadata record: nullopt if absent; IncompatibleSchema for an unknown
/// version; StoreCorruption if it cannot be decoded.
[[nodiscard]] std::optional<struct StoreMeta> read_meta(const storage::KvStore& store);

/// Key of the single metadata record in Table::Meta.
[[nodiscard]] Bytes meta_key();

struct StoreMeta {
    std::uint32_t schema_version = kSchemaVersion;
    std::string chain_id;
    Hash32 genesis_hash{};
    std::uint64_t last_height = 0;
    Hash32 last_state_root{};

    bool operator==(const StoreMeta&) const = default;
    void encode_to(ycs::Writer& w) const;
    static StoreMeta decode_from(ycs::Reader& r);
};

/// Results of one committed block, keyed by height (big-endian u64) in Table::Results.
struct BlockRecord {
    std::uint64_t height = 0;
    Hash32 state_root{};
    std::vector<std::uint32_t> codes;

    bool operator==(const BlockRecord&) const = default;
    void encode_to(ycs::Writer& w) const;
    static BlockRecord decode_from(ycs::Reader& r);
};

[[nodiscard]] Bytes results_key(std::uint64_t height);

/// state::KvReader view of one table of a KvStore (the base of execution overlays).
class TableReader final : public state::KvReader {
public:
    TableReader(const storage::KvStore& store, storage::Table table) : store_(&store), table_(table) {}
    [[nodiscard]] std::optional<Bytes> get(ByteView key) const override { return store_->get(table_, key); }
    void for_each(const std::function<void(const Bytes&, const Bytes&)>& fn) const override {
        store_->for_each(table_, fn);
    }

private:
    const storage::KvStore* store_;
    storage::Table table_;
};

/// Nodes of the persistent SMT stored in Table::Smt.
class StoreNodeReader final : public merkle::NodeReader {
public:
    explicit StoreNodeReader(const storage::KvStore& store) : store_(&store) {}
    [[nodiscard]] std::optional<merkle::SmtNode> node(const Hash32& hash) const override;

private:
    const storage::KvStore* store_;
};

/// Feeds every change of `block_state` (relative to its base) into `smt` and returns the new root.
Hash32 apply_state_changes(merkle::SmtUpdater& smt, const state::Overlay& block_state);

}  // namespace yap::chain
