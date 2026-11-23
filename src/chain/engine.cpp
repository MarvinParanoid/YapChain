// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/engine.hpp"

#include <stdexcept>

#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

namespace yap::chain {

using storage::Table;

namespace {
bool table_empty(const storage::KvStore& store, Table t) {
    bool empty = true;
    store.for_each(t, [&empty](const Bytes&, const Bytes&) { empty = false; });
    return empty;
}
}  // namespace

Engine::Engine(storage::KvStore& store) : store_(&store), state_view_(store, Table::State), nodes_(store) {
    meta_ = read_meta(*store_);  // throws IncompatibleSchema / StoreCorruption
    if (!meta_) {
        // A store without metadata must be completely empty; anything else is a damaged store
        // and is never reinitialised silently.
        for (std::size_t t = 0; t < storage::kTableCount; ++t) {
            if (!table_empty(*store_, static_cast<Table>(t))) {
                throw StoreCorruption("store has data but no metadata (table " +
                                      std::string(storage::table_name(static_cast<Table>(t))) + ")");
            }
        }
        return;
    }
    // Cheap consistency checks on every start; the full check is verify_state().
    state::Overlay view(&state_view_);
    const auto cs = state::read_value<state::ChainState>(view, state::key::chain_state());
    if (!cs) throw StoreCorruption("committed state has no ChainState");
    if (cs->height != meta_->last_height) {
        throw StoreCorruption("ChainState height " + std::to_string(cs->height) + " != metadata last_height " +
                              std::to_string(meta_->last_height));
    }
    if (meta_->last_state_root != Hash32{}) {
        const auto root = nodes_.node(meta_->last_state_root);
        if (!root || merkle::node_hash(*root) != meta_->last_state_root) {
            throw StoreCorruption("SMT root node does not match metadata last_state_root");
        }
    }
    if (meta_->last_height > 0) {
        const auto rec = block_record(meta_->last_height);
        if (!rec || rec->state_root != meta_->last_state_root) {
            throw StoreCorruption("block record of the last height is missing or disagrees with metadata");
        }
    }
}

void Engine::require_initialized() const {
    if (!meta_) throw std::logic_error("Engine: init_chain has not been called");
}

void Engine::require_fresh() const {
    require_initialized();
    const auto stored = store_->get(Table::Meta, meta_key());
    if (!stored || *stored != ycs::encode(*meta_)) {
        throw storage::StorageConflict("the store was advanced by another writer; reopen it");
    }
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

Hash32 Engine::init_chain(const Genesis& genesis, const storage::WriteBatch& extra) {
    if (meta_) throw std::logic_error("Engine: already initialised");
    if (auto err = validate_genesis(genesis); !err.empty()) throw std::invalid_argument("invalid genesis: " + err);

    state::Overlay o(&state_view_);
    write_initial_state(genesis, o);
    state::check_invariants(o);
    merkle::SmtUpdater smt(nodes_, Hash32{});
    const Hash32 root = apply_state_changes(smt, o);
    if (root != state::state_root(o)) throw FailStop("persistent SMT disagrees with the full recomputation");

    StoreMeta meta{kSchemaVersion, genesis.chain_id, genesis_hash(genesis), 0, root};
    storage::WriteBatch batch;
    batch.expect(Table::Meta, meta_key(), std::nullopt);  // nobody initialised it meanwhile
    append_state_changes(batch, o);
    append_smt_changes(batch, smt);
    batch.mark("after_smt");
    batch.put(Table::Meta, meta_key(), ycs::encode(meta));
    batch.put(Table::Results, results_key(0), ycs::encode(BlockRecord{0, root, {}}));  // genesis root
    for (const auto& op : extra.ops()) {
        if (op.kind == storage::WriteOp::Kind::Put) batch.put(op.table, op.key, op.value);
        if (op.kind == storage::WriteOp::Kind::Erase) batch.erase(op.table, op.key);
    }
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
    if (!rec) throw StoreCorruption("corrupted block record at height " + std::to_string(height));
    return std::move(rec).value();
}

CheckResult Engine::check_tx(ByteView tx_bytes) {
    require_fresh();
    if (!check_state_) check_state_ = std::make_unique<state::Overlay>(&state_view_);
    auto r = state::execute_tx(*check_state_, tx_bytes, meta_->last_height + 1, meta_->chain_id);
    return {r.code, std::move(r.log)};
}

std::vector<Bytes> Engine::prepare_proposal(const state::BlockContext& ctx, const std::vector<Bytes>& candidates,
                                            std::uint64_t max_bytes, std::vector<tx::TxCode>* codes) const {
    require_fresh();
    state::Overlay scratch(&state_view_);
    std::vector<Bytes> out;
    std::uint64_t used = 0;
    if (codes != nullptr) codes->clear();
    for (const auto& tx : candidates) {
        if (tx.size() > max_bytes - used) {  // used <= max_bytes always holds
            if (codes != nullptr) codes->push_back(tx::TxCode::ErrTooLarge);
            continue;
        }
        const auto code = state::execute_tx(scratch, tx, ctx.height, meta_->chain_id).code;
        if (codes != nullptr) codes->push_back(code);
        if (code != tx::TxCode::Ok) continue;
        used += tx.size();
        out.push_back(tx);
    }
    return out;
}

bool Engine::process_proposal(const state::BlockContext& ctx) const {
    require_fresh();
    state::Overlay scratch(&state_view_);  // discarded: ProcessProposal never changes the state
    merkle::SmtUpdater smt(nodes_, meta_->last_state_root);
    const auto result = state::execute_block(scratch, meta_->chain_id, ctx, incremental_root(smt));
    for (const auto& r : result.tx_results) {
        if (r.code != tx::TxCode::Ok) return false;
    }
    return true;
}

state::BlockResult Engine::finalize_block(const state::BlockContext& ctx) {
    require_fresh();
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

const StoreMeta& Engine::meta() const {
    require_initialized();
    return *meta_;
}

void Engine::abort_pending() noexcept {
    pending_.reset();
    pending_smt_.reset();
    pending_result_.reset();
}

Hash32 Engine::commit(const storage::WriteBatch& extra) {
    require_initialized();
    if (!pending_) throw std::logic_error("Engine: commit without finalize_block");

    StoreMeta next = *meta_;
    next.last_height = pending_height_;
    next.last_state_root = pending_result_->state_root;
    BlockRecord record{pending_height_, pending_result_->state_root, {}};
    for (const auto& r : pending_result_->tx_results) record.codes.push_back(static_cast<std::uint32_t>(r.code));

    storage::WriteBatch batch;
    // Compare-and-swap: the pending block was computed on top of exactly this committed height.
    batch.expect(Table::Meta, meta_key(), ycs::encode(*meta_));
    append_state_changes(batch, *pending_);
    append_smt_changes(batch, *pending_smt_);
    batch.mark("after_smt");
    batch.put(Table::Results, results_key(pending_height_), ycs::encode(record));
    batch.put(Table::Meta, meta_key(), ycs::encode(next));
    for (const auto& op : extra.ops()) {
        if (op.kind == storage::WriteOp::Kind::Put) batch.put(op.table, op.key, op.value);
        if (op.kind == storage::WriteOp::Kind::Erase) batch.erase(op.table, op.key);
    }

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
