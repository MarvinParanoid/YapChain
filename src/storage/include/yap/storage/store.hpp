// SPDX-License-Identifier: Apache-2.0
//
// Durable key-value storage (architecture.md §3.7, ADR-0007, ADR-0022).
//
// A store has a fixed set of tables. Reads are point lookups or ordered scans; writes are
// applied only as whole WriteBatches, atomically: after a crash either every operation of a
// batch is visible or none is. Keys and values are arbitrary bytes (keys are non-empty).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "yap/common/bytes.hpp"

namespace yap::storage {

/// Logical tables. Consensus state and SMT service nodes never share a table (ADR-0022).
enum class Table : std::uint8_t {
    State = 0,    ///< consensus state: raw_key -> YCS value (protocol.md §7.1)
    Smt = 1,      ///< sparse Merkle tree nodes: node hash -> encoded node
    Results = 2,  ///< per-height execution results
    Meta = 3,     ///< store metadata: schema version, chain id, last height and root
    Blocks = 4,   ///< dev-chain blocks by height (ADR-0025); empty on non-dev nodes
};
inline constexpr std::size_t kTableCount = 5;
[[nodiscard]] std::string_view table_name(Table t) noexcept;

/// Storage failure (I/O, corruption reported by the engine, ...). Never a transaction error.
class StorageError : public std::runtime_error {
public:
    explicit StorageError(const std::string& what, int code = 0) : std::runtime_error(what), code_(code) {}
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

/// The configured map size is exhausted (LMDB MDB_MAP_FULL). The failed batch was not applied.
class StorageFull : public StorageError {
public:
    using StorageError::StorageError;
};

/// A batch precondition (WriteBatch::expect) did not hold: another writer changed the store since
/// the caller read it. The batch was not applied.
class StorageConflict : public StorageError {
public:
    using StorageError::StorageError;
};

struct WriteOp {
    enum class Kind : std::uint8_t { Put, Erase, Marker, Expect };
    Kind kind = Kind::Put;
    Table table = Table::State;
    Bytes key;
    Bytes value;
    std::string marker;  ///< fault-injection point name for Kind::Marker
    bool expect_absent = false;  ///< Kind::Expect: the key must not exist (otherwise it must equal `value`)
};

/// Ordered list of writes applied atomically by KvStore::write(). Markers are no-ops that name
/// points inside the write transaction for fault injection (crash tests).
class WriteBatch {
public:
    void put(Table t, Bytes key, Bytes value);
    void erase(Table t, Bytes key);
    void mark(std::string name);
    /// Precondition checked inside the write transaction, before later operations are applied:
    /// the key must currently hold exactly `value` (or be absent for nullopt). On mismatch the
    /// whole batch fails with StorageConflict. This is what makes commits compare-and-swap.
    void expect(Table t, Bytes key, std::optional<Bytes> value);
    [[nodiscard]] const std::vector<WriteOp>& ops() const noexcept { return ops_; }
    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }

private:
    std::vector<WriteOp> ops_;
};

class KvStore {
public:
    virtual ~KvStore() = default;
    [[nodiscard]] virtual std::optional<Bytes> get(Table t, ByteView key) const = 0;
    /// Visits every entry of a table in ascending key order, from one consistent snapshot.
    virtual void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const = 0;
    /// Applies the batch atomically (all or nothing) and durably before returning.
    virtual void write(const WriteBatch& batch) = 0;
    /// A read-only view of one consistent point in time: every read through it sees the same
    /// committed state, whatever writers do meanwhile. Writing to it throws StorageError.
    /// The snapshot must not outlive the store it was taken from.
    [[nodiscard]] virtual std::unique_ptr<KvStore> snapshot() const = 0;
};

/// Non-owning read-only view of another store (used as the "snapshot of a snapshot", which is
/// the same point in time). Must not outlive the viewed store.
class ReadOnlyView final : public KvStore {
public:
    explicit ReadOnlyView(const KvStore& base) : base_(&base) {}
    [[nodiscard]] std::optional<Bytes> get(Table t, ByteView key) const override { return base_->get(t, key); }
    void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const override {
        base_->for_each(t, fn);
    }
    void write(const WriteBatch&) override { throw StorageError("snapshot is read-only"); }
    [[nodiscard]] std::unique_ptr<KvStore> snapshot() const override { return std::make_unique<ReadOnlyView>(*base_); }

private:
    const KvStore* base_;
};

/// In-memory implementation with the same semantics (minus durability).
class MemoryStore final : public KvStore {
public:
    [[nodiscard]] std::optional<Bytes> get(Table t, ByteView key) const override;
    void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const override;
    void write(const WriteBatch& batch) override;
    [[nodiscard]] std::unique_ptr<KvStore> snapshot() const override;

private:
    // Same order as std::less<Bytes> (unsigned lexicographic, as in LMDB), but via an explicit
    // memcmp: GCC 14 at -O3 reports a false -Wstringop-overread inside the inlined
    // std::lexicographical_compare_three_way of std::vector<unsigned char>. Transparent, so
    // lookups by ByteView need no copy.
    struct KeyLess {
        using is_transparent = void;
        bool operator()(ByteView a, ByteView b) const noexcept {
            const std::size_t n = std::min(a.size(), b.size());
            if (n != 0) {
                if (const int c = std::memcmp(a.data(), b.data(), n); c != 0) return c < 0;
            }
            return a.size() < b.size();
        }
    };
    std::map<Bytes, Bytes, KeyLess> tables_[kTableCount];
    bool read_only_ = false;
};

// ------------------------------------------------------------------ fault injection (tests only)

/// Called with the name of each fault point reached by write(): "before_txn", every batch
/// marker, "before_commit" and "after_commit". Production code never installs a hook.
using FaultHook = std::function<void(std::string_view point)>;
void set_fault_hook(FaultHook hook);
void fault_point(std::string_view point);

}  // namespace yap::storage
