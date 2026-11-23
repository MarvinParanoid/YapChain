// SPDX-License-Identifier: Apache-2.0
#include "yap/state/ledger.hpp"

#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"

namespace yap::state {

using tx::TxCode;

namespace {

const crypto::AccountId& treasury_id() {
    static const crypto::AccountId id = crypto::system_account_id(crypto::kTreasuryAccountName);
    return id;
}

/// Applies the effects of an already validated transaction. Returns Ok or ErrOverflow.
TxCode apply_effects(State& st, const tx::SignedTx& stx, std::vector<Event>& events) {
    const tx::TxBody& b = stx.body;
    const crypto::AccountId sender = b.sender();

    if (const auto* t = std::get_if<tx::Transfer>(&b.payload)) {
        const auto total = checked_add(t->amount, b.fee);
        if (!total || !st.debit(sender, *total)) return TxCode::ErrOverflow;
        if (!st.credit(t->to, t->amount)) return TxCode::ErrOverflow;
        events.push_back({"transfer",
                          {{"from", to_hex(sender)}, {"to", to_hex(t->to)}, {"amount", std::to_string(t->amount)}}});
    } else if (const auto* bn = std::get_if<tx::Burn>(&b.payload)) {
        const auto total = checked_add(bn->amount, b.fee);
        if (!total || !st.debit(sender, *total)) return TxCode::ErrOverflow;
        ChainState cs = st.chain_state();
        const auto burned = checked_add(cs.total_burned, bn->amount);
        const auto supply = checked_sub(cs.total_supply, bn->amount);
        if (!burned || !supply) return TxCode::ErrOverflow;
        cs.total_burned = *burned;
        cs.total_supply = *supply;
        st.put_chain_state(cs);
        events.push_back({"burn", {{"from", to_hex(sender)}, {"amount", std::to_string(bn->amount)}}});
    } else {
        return TxCode::ErrUnsupported;  // unreachable: rejected at T7 until M3
    }

    if (!settle_fee(st, b.fee, events)) return TxCode::ErrOverflow;

    // Read the sender again: earlier writes in this overlay are visible.
    AccountState s = st.account(sender).value_or(AccountState{});
    const auto nonce = checked_add(s.nonce, 1);
    if (!nonce) return TxCode::ErrOverflow;
    s.nonce = *nonce;
    st.put_account(sender, s);
    return TxCode::Ok;
}

}  // namespace

bool settle_fee(State& st, std::uint64_t fee, std::vector<Event>& events) {
    const tx::ChainParams p = st.params();
    const auto burn_part = mul_div_floor(fee, p.fee_burn_bps, tx::kBpsDenominator);
    if (!burn_part) return false;
    const std::uint64_t to_treasury = fee - *burn_part;  // burn_part <= fee because fee_burn_bps <= 10000
    if (!st.credit(treasury_id(), to_treasury)) return false;
    ChainState cs = st.chain_state();
    const auto burned = checked_add(cs.total_burned, *burn_part);
    const auto supply = checked_sub(cs.total_supply, *burn_part);
    if (!burned || !supply) return false;
    cs.total_burned = *burned;
    cs.total_supply = *supply;
    st.put_chain_state(cs);
    events.push_back({"fee", {{"amount", std::to_string(fee)},
                              {"burned", std::to_string(*burn_part)},
                              {"treasury", std::to_string(to_treasury)}}});
    return true;
}

TxResult execute_tx(Overlay& block_state, ByteView tx_bytes, std::uint64_t height, std::string_view chain_id) {
    Overlay tx_state(&block_state);  // §7.5: per-transaction overlay
    State st(tx_state);
    TxResult result;
    auto v = validate_tx(tx_bytes, height, chain_id, st);
    if (v.code != TxCode::Ok) {
        result.code = v.code;
        result.log = std::string(tx::to_string(v.code));
        return result;  // tx_state is discarded
    }
    std::vector<Event> events;
    const TxCode code = apply_effects(st, *v.tx, events);
    result.code = code;
    result.log = std::string(tx::to_string(code));
    if (code != TxCode::Ok) {
        return result;  // rollback: nothing was merged
    }
    tx_state.merge_into(block_state);
    result.events = std::move(events);
    return result;
}

}  // namespace yap::state
