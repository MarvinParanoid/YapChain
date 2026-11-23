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
    auto it = m.find(Bytes(key.begin(), key.end()));
    if (it == m.end()) return std::nullopt;
    return it->second;
}

void MemoryStore::for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const {
    for (const auto& [k, v] : tables_[static_cast<std::size_t>(t)]) fn(k, v);
}

void MemoryStore::write(const WriteBatch& batch) {
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
