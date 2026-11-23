// SPDX-License-Identifier: Apache-2.0
#include "yap/state/block.hpp"

#include "yap/common/hex.hpp"
#include "yap/state/state_root.hpp"

namespace yap::state {

BlockResult execute_block(Overlay& block_state, std::string_view chain_id, const BlockContext& ctx) {
    State st(block_state);

    // 1. Context.
    const ChainState before = st.chain_state();
    if (ctx.chain_id != chain_id) throw FailStop("block context: chain_id mismatch");
    if (ctx.height != before.height + 1) throw FailStop("block context: height is not last_height + 1");
    if (ctx.time_ms < before.last_time_ms) throw FailStop("block context: time goes backwards");
    validate_last_commit(st, ctx.height, ctx.last_commit);

    BlockResult result;
    // 2. Evidence: events only.
    for (const auto& m : ctx.misbehavior) {
        result.events.push_back({"validator_misbehavior",
                                 {{"validator", to_hex(m.validator_address)},
                                  {"type", std::to_string(m.type)},
                                  {"height", std::to_string(m.height)}}});
    }
    // 3. Transactions, strictly in block order.
    result.tx_results.reserve(ctx.txs.size());
    for (const auto& tx : ctx.txs) {
        result.tx_results.push_back(execute_tx(block_state, tx, ctx.height, chain_id));
    }
    // 4. Emission.
    apply_block_reward(st, ctx.last_commit, result.events);
    // 5. ChainState height and time (supply fields were updated by steps 3-4).
    ChainState cs = st.chain_state();
    cs.height = ctx.height;
    cs.last_time_ms = ctx.time_ms;
    st.put_chain_state(cs);
#ifndef NDEBUG
    check_invariants(block_state);  // protocol.md §7.3: after every block in debug builds
#endif
    // 6. Commitment.
    result.state_root = state_root(block_state);
    return result;
}

}  // namespace yap::state
