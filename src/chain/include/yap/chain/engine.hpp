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
    /// `extra` operations are written in the same atomic transaction (e.g. dev-chain config).
    Hash32 init_chain(const Genesis& genesis, const storage::WriteBatch& extra = {});

    [[nodiscard]] bool initialized() const noexcept { return meta_.has_value(); }
    /// Info: only durably committed data.
    [[nodiscard]] Info info() const;

    /// CheckTx against the mempool check-state (committed state + accepted transactions since the
    /// last commit) at height last_height + 1. The check-state resets on commit.
    CheckResult check_tx(ByteView tx_bytes);

    /// PrepareProposal: keeps, in order, the candidates that execute with OK on a scratch copy of the
    /// state and fit into `max_bytes` in total (protocol.md §10). `ctx.txs` is ignored.
    /// If `codes` is given it receives, per candidate, the code of its sequential execution
    /// (or ErrTooLarge if it did not fit the byte budget).
    [[nodiscard]] std::vector<Bytes> prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                                      std::uint64_t max_bytes,
                                                      std::vector<tx::TxCode>* codes = nullptr) const;

    /// ProcessProposal: ACCEPT iff every transaction executes with OK. Never modifies the state.
    [[nodiscard]] bool process_proposal(const state::BlockContext& ctx) const;

    /// FinalizeBlock: executes the decided block into a pending overlay; nothing is written.
    state::BlockResult finalize_block(const state::BlockContext& ctx);

    /// Commit: one atomic, durable store write for the pending height. Returns the new root.
    /// `extra` operations (e.g. the dev block itself) are written in the same transaction.
    Hash32 commit(const storage::WriteBatch& extra = {});

    /// Discards a finalized but uncommitted block (e.g. a dev block rejected after execution).
    void abort_pending() noexcept;
    [[nodiscard]] bool has_pending() const noexcept { return pending_ != nullptr; }
    [[nodiscard]] const StoreMeta& meta() const;
    [[nodiscard]] const storage::KvStore& store() const noexcept { return *store_; }

    /// Committed consensus state (Table::State) as a read-only view.
    [[nodiscard]] const state::KvReader& committed() const noexcept { return state_view_; }
    [[nodiscard]] std::optional<BlockRecord> block_record(std::uint64_t height) const;

private:
    void require_initialized() const;
    /// Throws StorageConflict if another writer advanced the store since this engine read it.
    void require_fresh() const;
    void append_state_changes(storage::WriteBatch& batch, const state::Overlay& changes) const;
    static void append_smt_changes(storage::WriteBatch& batch, const merkle::SmtUpdater& smt);
    [[nodiscard]] state::RootFn incremental_root(merkle::SmtUpdater& smt) const;

    storage::KvStore* store_;
    TableReader state_view_;
    StoreNodeReader nodes_;
    std::unique_ptr<merkle::SmtUpdater> pending_smt_;
    std::optional<StoreMeta> meta_;
    std::unique_ptr<state::Overlay> pending_;
    std::optional<state::BlockResult> pending_result_;
    std::uint64_t pending_height_ = 0;
    std::unique_ptr<state::Overlay> check_state_;
};

}  // namespace yap::chain
