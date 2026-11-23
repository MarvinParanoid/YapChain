// SPDX-License-Identifier: Apache-2.0
//
// Execution engine implementing the Application lifecycle of architecture.md §4 (the ABCI++
// shape) over a durable KvStore. The CometBFT adapter (M2) will drive this class.
//
// Durability contract (consensus.md §6, ADR-0022):
//   finalize_block() executes and keeps the result pending in memory — nothing is written;
//   commit() writes the whole height (state changes, results, last height/root, metadata)
//   as ONE store transaction and returns only after it is durable. If the write fails, the
//   previous committed height stays current and the pending block is kept.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "yap/chain/genesis.hpp"
#include "yap/chain/store_layout.hpp"
#include "yap/state/block.hpp"
#include "yap/storage/store.hpp"

namespace yap::chain {

struct Info {
    std::string chain_id;
    std::uint64_t last_height = 0;
    Hash32 last_state_root{};
};

struct CheckResult {
    tx::TxCode code = tx::TxCode::ErrDecode;
    std::string log;
};

class Engine {
public:
    /// Attaches to `store`. An empty store is uninitialised (call init_chain); a store with
    /// metadata resumes from its last committed height.
    explicit Engine(storage::KvStore& store);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// InitChain: validates `genesis`, writes the initial state atomically, returns app_hash.
    /// Throws std::invalid_argument for an invalid genesis and std::logic_error if initialised.
    Hash32 init_chain(const Genesis& genesis);

    [[nodiscard]] bool initialized() const noexcept { return meta_.has_value(); }
    /// Info: only durably committed data.
    [[nodiscard]] Info info() const;

    /// CheckTx against the mempool check-state (committed state + accepted transactions since the
    /// last commit) at height last_height + 1. The check-state resets on commit.
    CheckResult check_tx(ByteView tx_bytes);

    /// PrepareProposal: keeps, in order, the candidates that execute with OK on a scratch copy of the
    /// state and fit into `max_bytes` in total (protocol.md §10). `ctx.txs` is ignored.
    [[nodiscard]] std::vector<Bytes> prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                                      std::uint64_t max_bytes) const;

    /// ProcessProposal: ACCEPT iff every transaction executes with OK. Never modifies the state.
    [[nodiscard]] bool process_proposal(const state::BlockContext& ctx) const;

    /// FinalizeBlock: executes the decided block into a pending overlay; nothing is written.
    state::BlockResult finalize_block(const state::BlockContext& ctx);

    /// Commit: one atomic, durable store write for the pending height. Returns the new root.
    Hash32 commit();

    /// Committed consensus state (Table::State) as a read-only view.
    [[nodiscard]] const state::KvReader& committed() const noexcept { return state_view_; }
    [[nodiscard]] std::optional<BlockRecord> block_record(std::uint64_t height) const;

private:
    void require_initialized() const;
    void append_state_changes(storage::WriteBatch& batch, const state::Overlay& changes) const;

    storage::KvStore* store_;
    TableReader state_view_;
    std::optional<StoreMeta> meta_;
    std::unique_ptr<state::Overlay> pending_;
    std::optional<state::BlockResult> pending_result_;
    std::uint64_t pending_height_ = 0;
    std::unique_ptr<state::Overlay> check_state_;
};

}  // namespace yap::chain
