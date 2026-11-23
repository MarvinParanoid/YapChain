// SPDX-License-Identifier: Apache-2.0
//
// Durable key-value storage (architecture.md §3.7, ADR-0007, ADR-0022).
//
// A store has a fixed set of tables. Reads are point lookups or ordered scans; writes are
// applied only as whole WriteBatches, atomically: after a crash either every operation of a
// batch is visible or none is. Keys and values are arbitrary bytes (keys are non-empty).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
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

struct WriteOp {
    enum class Kind : std::uint8_t { Put, Erase, Marker };
    Kind kind = Kind::Put;
    Table table = Table::State;
    Bytes key;
    Bytes value;
    std::string marker;  ///< fault-injection point name for Kind::Marker
};

/// Ordered list of writes applied atomically by KvStore::write(). Markers are no-ops that name
/// points inside the write transaction for fault injection (crash tests).
class WriteBatch {
public:
    void put(Table t, Bytes key, Bytes value);
    void erase(Table t, Bytes key);
    void mark(std::string name);
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
};

/// In-memory implementation with the same semantics (minus durability).
class MemoryStore final : public KvStore {
public:
    [[nodiscard]] std::optional<Bytes> get(Table t, ByteView key) const override;
    void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const override;
    void write(const WriteBatch& batch) override;

private:
    std::map<Bytes, Bytes> tables_[kTableCount];
};

// ------------------------------------------------------------------ fault injection (tests only)

/// Called with the name of each fault point reached by write(): "before_txn", every batch
/// marker, "before_commit" and "after_commit". Production code never installs a hook.
using FaultHook = std::function<void(std::string_view point)>;
void set_fault_hook(FaultHook hook);
void fault_point(std::string_view point);

}  // namespace yap::storage
