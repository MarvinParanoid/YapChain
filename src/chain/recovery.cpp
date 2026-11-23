// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/recovery.hpp"

#include <algorithm>

#include "yap/common/hex.hpp"
#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

namespace yap::chain {

using storage::Table;

VerifyReport verify_state(const storage::KvStore& store) {
    const auto meta = read_meta(store);  // IncompatibleSchema / StoreCorruption
    if (!meta) throw StoreCorruption("verify_state: store has no metadata");
    VerifyReport rep;
    rep.height = meta->last_height;
    rep.state_root = meta->last_state_root;

    const TableReader state_view(store, Table::State);
    try {
        // Supply invariants and decodable ChainState / params / accounts (§7.3).
        state::check_invariants(state_view);
        state::Overlay view(&state_view);
        if (state::State(view).chain_state().height != meta->last_height) {
            throw StoreCorruption("ChainState height disagrees with metadata");
        }
    } catch (const FailStop& e) {
        throw StoreCorruption(std::string("state invariants: ") + e.what());
    }

    // Complete SMT check, then leaves == consensus state entries.
    const StoreNodeReader nodes(store);
    merkle::TreeReport tree;
    try {
        tree = merkle::verify_tree(nodes, meta->last_state_root);
    } catch (const merkle::SmtCorruption& e) {
        throw StoreCorruption(std::string("SMT: ") + e.what());
    }
    std::vector<merkle::SmtLeaf> expected;
    state_view.for_each([&](const Bytes& k, const Bytes& v) {
        expected.push_back({merkle::smt_key(k), merkle::smt_value_hash(v)});
    });
    rep.state_entries = expected.size();
    std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    if (expected != tree.leaves) throw StoreCorruption("SMT leaves do not match the consensus state");

    std::size_t stored_nodes = 0;
    store.for_each(Table::Smt, [&stored_nodes](const Bytes&, const Bytes&) { ++stored_nodes; });
    if (stored_nodes != tree.leaves.size() + tree.internal_nodes) {
        throw StoreCorruption("SMT table holds " + std::to_string(stored_nodes) + " nodes, " +
                              std::to_string(tree.leaves.size() + tree.internal_nodes) + " are reachable");
    }
    // Independent full recomputation must give the same root.
    if (state::state_root(state_view) != meta->last_state_root) {
        throw StoreCorruption("recomputed state root differs from metadata last_state_root");
    }
    // The last block record agrees with the metadata.
    if (meta->last_height > 0) {
        const auto raw = store.get(Table::Results, results_key(meta->last_height));
        if (!raw) throw StoreCorruption("block record of the last height is missing");
        const auto rec = ycs::decode<BlockRecord>(*raw);
        if (!rec || rec->state_root != meta->last_state_root || rec->height != meta->last_height) {
            throw StoreCorruption("block record of the last height is missing or inconsistent");
        }
    }
    rep.smt_leaves = tree.leaves.size();
    rep.smt_internal_nodes = tree.internal_nodes;
    return rep;
}

std::size_t replay(Engine& engine, std::span<const ReplayBlock> blocks,
                   const std::function<void(const ReplayBlock&, const state::BlockResult&)>& on_block) {
    std::size_t executed = 0;
    for (const auto& b : blocks) {
        const std::uint64_t last = engine.info().last_height;
        if (b.ctx.height <= last) continue;  // already durable: never re-applied
        if (b.ctx.height != last + 1) {
            throw FailStop("replay: journal jumps from height " + std::to_string(last) + " to " +
                           std::to_string(b.ctx.height));
        }
        const auto result = engine.finalize_block(b.ctx);
        if (b.expected_root && *b.expected_root != result.state_root) {
            // Do not commit a diverging height.
            throw FailStop("replay: state root mismatch at height " + std::to_string(b.ctx.height) + ": got " +
                           to_hex(result.state_root) + ", journal says " + to_hex(*b.expected_root));
        }
        engine.commit();
        if (on_block) on_block(b, result);
        ++executed;
    }
    return executed;
}

}  // namespace yap::chain
