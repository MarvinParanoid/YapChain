// SPDX-License-Identifier: Apache-2.0
#include "yap/state/kv.hpp"

#include <stdexcept>

namespace yap::state {

std::optional<Bytes> MemoryKvStore::get(ByteView key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

void MemoryKvStore::for_each(const std::function<void(const Bytes&, const Bytes&)>& fn) const {
    for (const auto& [k, v] : data_) fn(k, v);
}

void MemoryKvStore::put(Bytes key, Bytes value) { data_[std::move(key)] = std::move(value); }
void MemoryKvStore::erase(const Bytes& key) { data_.erase(key); }

std::optional<Bytes> Overlay::get(ByteView key) const {
    auto it = changes_.find(key);
    if (it != changes_.end()) return it->second;
    return base_->get(key);
}

void Overlay::for_each(const std::function<void(const Bytes&, const Bytes&)>& fn) const {
    // Merge two ordered sequences: base entries and local changes (local wins).
    std::map<Bytes, Bytes, BytesLess> merged;
    base_->for_each([&merged](const Bytes& k, const Bytes& v) { merged.emplace(k, v); });
    for (const auto& [k, v] : changes_) {
        if (v) {
            merged[k] = *v;
        } else {
            merged.erase(k);
        }
    }
    for (const auto& [k, v] : merged) fn(k, v);
}

void Overlay::put(Bytes key, Bytes value) { changes_[std::move(key)] = std::move(value); }
void Overlay::erase(Bytes key) { changes_[std::move(key)] = std::nullopt; }

void Overlay::merge_into(Overlay& parent) {
    if (base_ != &parent) {
        throw std::logic_error("Overlay::merge_into: parent is not the base of this overlay");
    }
    for (auto& [k, v] : changes_) parent.changes_[k] = std::move(v);
    changes_.clear();
}

void Overlay::apply_to(MemoryKvStore& store) {
    if (base_ != &store) {
        throw std::logic_error("Overlay::apply_to: store is not the base of this overlay");
    }
    for (auto& [k, v] : changes_) {
        if (v) {
            store.put(k, std::move(*v));
        } else {
            store.erase(k);
        }
    }
    changes_.clear();
}

}  // namespace yap::state
