// SPDX-License-Identifier: Apache-2.0
//
// Ordered key-value views used by the state machine. Keys and values are raw bytes;
// iteration is always in lexicographic byte order (never hash order).
#pragma once

#include <functional>
#include <map>
#include <optional>

#include "yap/common/bytes.hpp"

namespace yap::state {

class KvReader {
public:
    virtual ~KvReader() = default;
    [[nodiscard]] virtual std::optional<Bytes> get(ByteView key) const = 0;
    /// Visits every live entry in ascending key order.
    virtual void for_each(const std::function<void(const Bytes& key, const Bytes& value)>& fn) const = 0;
};

/// Committed in-memory store (persistence arrives in M1.3).
class MemoryKvStore final : public KvReader {
public:
    [[nodiscard]] std::optional<Bytes> get(ByteView key) const override;
    void for_each(const std::function<void(const Bytes&, const Bytes&)>& fn) const override;
    void put(Bytes key, Bytes value);
    void erase(const Bytes& key);
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

private:
    std::map<Bytes, Bytes> data_;
};

/// Buffer of changes on top of a base view (docs/protocol.md §7.5). Reads see own writes first.
/// Nothing reaches the base until the overlay is explicitly merged.
class Overlay final : public KvReader {
public:
    /// Layers a new, empty overlay on top of `base`. The base is passed by pointer on purpose:
    /// with a reference parameter, `Overlay inner(outer)` would silently pick the copy
    /// constructor and write into a copy instead of a layer.
    explicit Overlay(const KvReader* base) : base_(base) {}
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) = delete;
    Overlay& operator=(Overlay&&) = delete;
    ~Overlay() override = default;

    [[nodiscard]] std::optional<Bytes> get(ByteView key) const override;
    void for_each(const std::function<void(const Bytes&, const Bytes&)>& fn) const override;
    void put(Bytes key, Bytes value);
    void erase(Bytes key);

    /// Moves all changes into `parent` (whose base must be this overlay's base) and clears this one.
    void merge_into(Overlay& parent);
    /// Applies all changes to a store (which must be this overlay's base) and clears this one.
    void apply_to(MemoryKvStore& store);
    [[nodiscard]] bool empty() const noexcept { return changes_.empty(); }
    /// The pending changes in ascending key order (nullopt = deletion).
    [[nodiscard]] const std::map<Bytes, std::optional<Bytes>>& changes() const noexcept { return changes_; }
    [[nodiscard]] const KvReader* base() const noexcept { return base_; }

private:
    const KvReader* base_;
    std::map<Bytes, std::optional<Bytes>> changes_;  // nullopt = deleted
};

}  // namespace yap::state
