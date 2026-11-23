// SPDX-License-Identifier: Apache-2.0
//
// Store verification and block replay (ADR-0023, consensus.md §6).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "yap/chain/engine.hpp"

namespace yap::chain {

struct VerifyReport {
    std::uint64_t height = 0;
    Hash32 state_root{};
    std::size_t state_entries = 0;
    std::size_t smt_leaves = 0;
    std::size_t smt_internal_nodes = 0;
};

/// Full logical verification of a store (the `verify-state` operation):
/// metadata, ChainState height, block record of the last height, the complete SMT
/// (verify_tree), leaves == consensus state, no orphan nodes, root == metadata ==
/// full recomputation, supply invariants. Throws StoreCorruption on the first problem.
/// Read-only: never modifies the store.
[[nodiscard]] VerifyReport verify_state(const storage::KvStore& store);

struct ReplayBlock {
    state::BlockContext ctx;
    std::optional<Hash32> expected_root;  ///< if present, a different root is a fatal divergence
};

/// Replays `blocks` (ascending heights) through `engine`: heights already committed are skipped,
/// so a restart never applies a block — or its emission — twice. A gap in heights, or a root that
/// differs from `expected_root`, throws FailStop and leaves the store at the last good height.
/// Returns the number of blocks executed.
std::size_t replay(Engine& engine, std::span<const ReplayBlock> blocks,
                   const std::function<void(const ReplayBlock&, const state::BlockResult&)>& on_block = {});

}  // namespace yap::chain
