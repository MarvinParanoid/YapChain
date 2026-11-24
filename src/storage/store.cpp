// SPDX-License-Identifier: Apache-2.0
#include "yap/storage/store.hpp"

#include <utility>

namespace yap::storage {

std::string_view table_name(Table t) noexcept {
    switch (t) {
        case Table::State: return "state";
        case Table::Smt: return "smt";
        case Table::Results: return "results";
        case Table::Meta: return "meta";
        case Table::Blocks: return "blocks";
    }
    return "unknown";
}

void WriteBatch::put(Table t, Bytes key, Bytes value) {
    if (key.empty()) throw std::invalid_argument("WriteBatch: empty key");
    ops_.push_back({WriteOp::Kind::Put, t, std::move(key), std::move(value), {}});
}

void WriteBatch::erase(Table t, Bytes key) {
    if (key.empty()) throw std::invalid_argument("WriteBatch: empty key");
    ops_.push_back({WriteOp::Kind::Erase, t, std::move(key), {}, {}});
}

void WriteBatch::expect(Table t, Bytes key, std::optional<Bytes> value) {
    if (key.empty()) throw std::invalid_argument("WriteBatch: empty key");
    WriteOp op{WriteOp::Kind::Expect, t, std::move(key), value.value_or(Bytes{}), {}};
    op.expect_absent = !value.has_value();
    ops_.push_back(std::move(op));
}

void WriteBatch::mark(std::string name) { ops_.push_back({WriteOp::Kind::Marker, Table::State, {}, {}, std::move(name)}); }

namespace {
FaultHook& hook_storage() {
    static FaultHook hook;
    return hook;
}
}  // namespace

void set_fault_hook(FaultHook hook) { hook_storage() = std::move(hook); }

void fault_point(std::string_view point) {
    if (const auto& h = hook_storage()) h(point);
}

std::optional<Bytes> MemoryStore::get(Table t, ByteView key) const {
    const auto& m = tables_[static_cast<std::size_t>(t)];
    auto it = m.find(key);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

void MemoryStore::for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const {
    for (const auto& [k, v] : tables_[static_cast<std::size_t>(t)]) fn(k, v);
}

std::unique_ptr<KvStore> MemoryStore::snapshot() const {
    if (read_only_) return std::make_unique<ReadOnlyView>(*this);  // already a frozen copy
    auto copy = std::make_unique<MemoryStore>(*this);
    copy->read_only_ = true;
    return copy;
}

void MemoryStore::write(const WriteBatch& batch) {
    if (read_only_) throw StorageError("snapshot is read-only");
    fault_point("before_txn");
    // Undo log: if anything throws mid-batch (allocation, a test hook), restore the previous
    // contents so the batch is all-or-nothing, like an aborted LMDB transaction.
    struct Undo {
        std::size_t table;
        Bytes key;
        std::optional<Bytes> previous;
    };
    std::vector<Undo> undo;
    try {
        for (const auto& op : batch.ops()) {
            if (op.kind == WriteOp::Kind::Marker) {
                fault_point(op.marker);
                continue;
            }
            if (op.kind == WriteOp::Kind::Expect) {
                const auto& m = tables_[static_cast<std::size_t>(op.table)];
                const auto it = m.find(op.key);
                const bool ok = op.expect_absent ? it == m.end() : (it != m.end() && it->second == op.value);
                if (!ok) throw StorageConflict("store changed by another writer (precondition failed)");
                continue;
            }
            const auto t = static_cast<std::size_t>(op.table);
            auto& m = tables_[t];
            auto it = m.find(op.key);
            undo.push_back({t, op.key, it == m.end() ? std::nullopt : std::optional<Bytes>(it->second)});
            if (op.kind == WriteOp::Kind::Put) {
                m[op.key] = op.value;
            } else if (it != m.end()) {
                m.erase(it);
            }
        }
        fault_point("before_commit");
    } catch (...) {
        for (auto u = undo.rbegin(); u != undo.rend(); ++u) {
            if (u->previous) {
                tables_[u->table][u->key] = *u->previous;
            } else {
                tables_[u->table].erase(u->key);
            }
        }
        throw;
    }
    fault_point("after_commit");
}

}  // namespace yap::storage
