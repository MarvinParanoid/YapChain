// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/engine.hpp"

#include <stdexcept>

#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

namespace yap::chain {

void Engine::require_initialized() const {
    if (!initialized_) throw std::logic_error("Engine: init_chain has not been called");
}

Hash32 Engine::init_chain(const Genesis& genesis) {
    if (initialized_ || !committed_.empty()) throw std::logic_error("Engine: already initialised");
    if (auto err = validate_genesis(genesis); !err.empty()) throw std::invalid_argument("invalid genesis: " + err);
    state::Overlay o(&committed_);
    write_initial_state(genesis, o);
    state::check_invariants(o);
    o.apply_to(committed_);
    chain_id_ = genesis.chain_id;
    last_state_root_ = state::state_root(committed_);
    initialized_ = true;
    return last_state_root_;
}

Info Engine::info() const {
    require_initialized();
    state::Overlay view(&committed_);
    return {chain_id_, state::State(view).chain_state().height, last_state_root_};
}

CheckResult Engine::check_tx(ByteView tx_bytes) {
    require_initialized();
    if (!check_state_) check_state_ = std::make_unique<state::Overlay>(&committed_);
    const std::uint64_t height = info().last_height + 1;
    auto r = state::execute_tx(*check_state_, tx_bytes, height, chain_id_);
    return {r.code, std::move(r.log)};
}

std::vector<Bytes> Engine::prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                            std::uint64_t max_bytes) const {
    require_initialized();
    state::Overlay scratch(&committed_);
    std::vector<Bytes> out;
    std::uint64_t used = 0;
    for (const auto& tx : candidates) {
        if (tx.size() > max_bytes - used) continue;  // used <= max_bytes always holds
        if (state::execute_tx(scratch, tx, ctx.height, chain_id_).code != tx::TxCode::Ok) continue;
        used += tx.size();
        out.push_back(tx);
    }
    return out;
}

bool Engine::process_proposal(const state::BlockContext& ctx) const {
    require_initialized();
    state::Overlay scratch(&committed_);  // discarded: ProcessProposal never changes the state
    const auto result = state::execute_block(scratch, chain_id_, ctx);
    for (const auto& r : result.tx_results) {
        if (r.code != tx::TxCode::Ok) return false;
    }
    return true;
}

state::BlockResult Engine::finalize_block(const state::BlockContext& ctx) {
    require_initialized();
    if (pending_) throw std::logic_error("Engine: finalize_block called twice without commit");
    auto overlay = std::make_unique<state::Overlay>(&committed_);
    auto result = state::execute_block(*overlay, chain_id_, ctx);  // throws FailStop: nothing kept
    pending_ = std::move(overlay);
    pending_result_ = result;
    return result;
}

Hash32 Engine::commit() {
    require_initialized();
    if (!pending_) throw std::logic_error("Engine: commit without finalize_block");
    pending_->apply_to(committed_);
    pending_.reset();
    last_state_root_ = pending_result_->state_root;
    pending_result_.reset();
    check_state_.reset();  // mempool transactions are re-checked against the new state
    return last_state_root_;
}

}  // namespace yap::chain
