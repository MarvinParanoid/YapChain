// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/engine.hpp"

#include <stdexcept>

#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

namespace yap::chain {

using storage::Table;

Engine::Engine(storage::KvStore& store) : store_(&store), state_view_(store, Table::State), nodes_(store) {
    if (auto raw = store_->get(Table::Meta, meta_key())) {
        auto meta = ycs::decode<StoreMeta>(*raw);
        if (!meta) throw storage::StorageError("store metadata is unreadable or has an unsupported schema version");
        meta_ = std::move(meta).value();
    }
}

void Engine::require_initialized() const {
    if (!meta_) throw std::logic_error("Engine: init_chain has not been called");
}

void Engine::append_state_changes(storage::WriteBatch& batch, const state::Overlay& changes) const {
    // Deterministic order with named fault points: accounts, ChainState, everything else.
    std::vector<const std::pair<const Bytes, std::optional<Bytes>>*> accounts;
    for (const auto& kv : changes.changes()) {
        if (kv.first[0] == state::ns::kAccount) accounts.push_back(&kv);
    }
    auto emit = [&batch](const Bytes& k, const std::optional<Bytes>& v) {
        if (v) {
            batch.put(Table::State, k, *v);
        } else {
            batch.erase(Table::State, k);
        }
    };
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        if (i == accounts.size() / 2) batch.mark("mid_accounts");
        emit(accounts[i]->first, accounts[i]->second);
    }
    batch.mark("after_accounts");
    for (const auto& [k, v] : changes.changes()) {
        if (k[0] == state::ns::kChainState) emit(k, v);
    }
    batch.mark("after_chain_state");
    for (const auto& [k, v] : changes.changes()) {
        if (k[0] != state::ns::kAccount && k[0] != state::ns::kChainState) emit(k, v);
    }
}

void Engine::append_smt_changes(storage::WriteBatch& batch, const merkle::SmtUpdater& smt) {
    for (const auto& [hash, node] : smt.changes()) {
        if (node) {
            batch.put(Table::Smt, Bytes(hash.begin(), hash.end()), merkle::encode_node(*node));
        } else {
            batch.erase(Table::Smt, Bytes(hash.begin(), hash.end()));
        }
    }
}

state::RootFn Engine::incremental_root(merkle::SmtUpdater& smt) const {
    return [&smt](const state::Overlay& block_state) { return apply_state_changes(smt, block_state); };
}

Hash32 Engine::init_chain(const Genesis& genesis) {
    if (meta_) throw std::logic_error("Engine: already initialised");
    bool empty = true;
    for (std::size_t t = 0; t < storage::kTableCount && empty; ++t) {
        store_->for_each(static_cast<Table>(t), [&empty](const Bytes&, const Bytes&) { empty = false; });
    }
    if (!empty) throw storage::StorageError("refusing to initialise a non-empty store without metadata");
    if (auto err = validate_genesis(genesis); !err.empty()) throw std::invalid_argument("invalid genesis: " + err);

    state::Overlay o(&state_view_);
    write_initial_state(genesis, o);
    state::check_invariants(o);
    merkle::SmtUpdater smt(nodes_, Hash32{});
    const Hash32 root = apply_state_changes(smt, o);
    if (root != state::state_root(o)) throw FailStop("persistent SMT disagrees with the full recomputation");

    StoreMeta meta{kSchemaVersion, genesis.chain_id, genesis_hash(genesis), 0, root};
    storage::WriteBatch batch;
    append_state_changes(batch, o);
    append_smt_changes(batch, smt);
    batch.mark("after_smt");
    batch.put(Table::Meta, meta_key(), ycs::encode(meta));
    store_->write(batch);  // durable before InitChain returns
    meta_ = meta;
    return root;
}

Info Engine::info() const {
    require_initialized();
    return {meta_->chain_id, meta_->last_height, meta_->last_state_root};
}

std::optional<BlockRecord> Engine::block_record(std::uint64_t height) const {
    auto raw = store_->get(Table::Results, results_key(height));
    if (!raw) return std::nullopt;
    auto rec = ycs::decode<BlockRecord>(*raw);
    if (!rec) throw storage::StorageError("corrupted block record");
    return std::move(rec).value();
}

CheckResult Engine::check_tx(ByteView tx_bytes) {
    require_initialized();
    if (!check_state_) check_state_ = std::make_unique<state::Overlay>(&state_view_);
    auto r = state::execute_tx(*check_state_, tx_bytes, meta_->last_height + 1, meta_->chain_id);
    return {r.code, std::move(r.log)};
}

std::vector<Bytes> Engine::prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                            std::uint64_t max_bytes) const {
    require_initialized();
    state::Overlay scratch(&state_view_);
    std::vector<Bytes> out;
    std::uint64_t used = 0;
    for (const auto& tx : candidates) {
        if (tx.size() > max_bytes - used) continue;  // used <= max_bytes always holds
        if (state::execute_tx(scratch, tx, ctx.height, meta_->chain_id).code != tx::TxCode::Ok) continue;
        used += tx.size();
        out.push_back(tx);
    }
    return out;
}

bool Engine::process_proposal(const state::BlockContext& ctx) const {
    require_initialized();
    state::Overlay scratch(&state_view_);  // discarded: ProcessProposal never changes the state
    merkle::SmtUpdater smt(nodes_, meta_->last_state_root);
    const auto result = state::execute_block(scratch, meta_->chain_id, ctx, incremental_root(smt));
    for (const auto& r : result.tx_results) {
        if (r.code != tx::TxCode::Ok) return false;
    }
    return true;
}

state::BlockResult Engine::finalize_block(const state::BlockContext& ctx) {
    require_initialized();
    if (pending_) throw std::logic_error("Engine: finalize_block called twice without commit");
    auto overlay = std::make_unique<state::Overlay>(&state_view_);
    auto smt = std::make_unique<merkle::SmtUpdater>(nodes_, meta_->last_state_root);
    // Throws FailStop (or SmtCorruption): nothing is kept.
    auto result = state::execute_block(*overlay, meta_->chain_id, ctx, incremental_root(*smt));
    pending_ = std::move(overlay);
    pending_smt_ = std::move(smt);
    pending_result_ = result;
    pending_height_ = ctx.height;
    return result;
}

Hash32 Engine::commit() {
    require_initialized();
    if (!pending_) throw std::logic_error("Engine: commit without finalize_block");

    StoreMeta next = *meta_;
    next.last_height = pending_height_;
    next.last_state_root = pending_result_->state_root;
    BlockRecord record{pending_height_, pending_result_->state_root, {}};
    for (const auto& r : pending_result_->tx_results) record.codes.push_back(static_cast<std::uint32_t>(r.code));

    storage::WriteBatch batch;
    append_state_changes(batch, *pending_);
    append_smt_changes(batch, *pending_smt_);
    batch.mark("after_smt");
    batch.put(Table::Results, results_key(pending_height_), ycs::encode(record));
    batch.put(Table::Meta, meta_key(), ycs::encode(next));

    store_->write(batch);  // throws on failure: meta_ and pending_ are left untouched

    // Only now is the height committed.
    meta_ = next;
    pending_.reset();
    pending_smt_.reset();
    pending_result_.reset();
    check_state_.reset();  // mempool transactions are re-checked against the new state
    return next.last_state_root;
}

}  // namespace yap::chain
