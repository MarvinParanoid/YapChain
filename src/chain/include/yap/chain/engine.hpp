// SPDX-License-Identifier: Apache-2.0
//
// In-memory execution engine implementing the Application lifecycle of architecture.md §4
// (the ABCI++ shape) over the deterministic state machine. Persistence arrives in M1.3, the
// CometBFT adapter in M2; this class is what both will drive.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "yap/chain/genesis.hpp"
#include "yap/state/block.hpp"

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
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// InitChain: validates `genesis`, writes the initial state, returns its root (app_hash).
    /// Throws std::invalid_argument for an invalid genesis and std::logic_error if already initialised.
    Hash32 init_chain(const Genesis& genesis);

    [[nodiscard]] Info info() const;

    /// CheckTx against the mempool check-state (committed state + transactions accepted since the
    /// last commit), at height last_height + 1. Accepted transactions are applied to the check-state
    /// so that consecutive nonces from one sender are accepted. The check-state resets on commit.
    CheckResult check_tx(ByteView tx_bytes);

    /// PrepareProposal: keeps, in order, the candidates that execute with OK on a scratch copy of the
    /// state and fit into `max_bytes` in total (protocol.md §10). `ctx.txs` is ignored.
    [[nodiscard]] std::vector<Bytes> prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                                      std::uint64_t max_bytes) const;

    /// ProcessProposal: ACCEPT iff every transaction executes with OK. Never modifies the state.
    [[nodiscard]] bool process_proposal(const state::BlockContext& ctx) const;

    /// FinalizeBlock: executes the decided block on a pending overlay (nothing is committed yet).
    state::BlockResult finalize_block(const state::BlockContext& ctx);

    /// Commit: applies the pending block. Returns the new state root.
    Hash32 commit();

    /// Read-only view of the committed state (for tests and queries).
    [[nodiscard]] const state::KvReader& committed() const noexcept { return committed_; }

private:
    void require_initialized() const;

    std::string chain_id_;
    state::MemoryKvStore committed_;
    std::unique_ptr<state::Overlay> pending_;
    std::optional<state::BlockResult> pending_result_;
    std::unique_ptr<state::Overlay> check_state_;
    Hash32 last_state_root_{};
    bool initialized_ = false;
};

}  // namespace yap::chain
