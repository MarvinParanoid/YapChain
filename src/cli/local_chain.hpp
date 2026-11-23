// SPDX-License-Identifier: Apache-2.0
// Access to the local chain database (<home>/data, LMDB).
#pragma once

#include <memory>
#include <optional>

#include "context.hpp"
#include "yap/chain/engine.hpp"
#include "yap/storage/lmdb_store.hpp"

namespace yap::cli {

struct LocalChain {
    std::unique_ptr<storage::LmdbStore> store;
    std::unique_ptr<storage::KvStore> snapshot;  ///< read-only opens: every query reads one snapshot
    std::unique_ptr<chain::Engine> engine;

    [[nodiscard]] state::ChainState chain_state() const;
    [[nodiscard]] tx::ChainParams params() const;
    [[nodiscard]] std::optional<state::AccountState> account(const crypto::AccountId& id) const;
};

/// Opens <home>/data. Returns nullopt if there is no database yet (when `required` is false);
/// throws CliError otherwise. Storage failures propagate as StorageError.
[[nodiscard]] std::optional<LocalChain> open_local_chain(const Context& ctx, bool read_only, bool required);

}  // namespace yap::cli
