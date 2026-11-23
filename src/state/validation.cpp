// SPDX-License-Identifier: Apache-2.0
#include "yap/state/validation.hpp"

#include "yap/common/checked.hpp"
#include "yap/tx/stateless.hpp"

namespace yap::state {

using tx::TxCode;

TxCode check_stateful(const tx::SignedTx& stx, std::uint64_t height, const StateReader& state) {
    const tx::TxBody& b = stx.body;
    const tx::ChainParams params = state.params();

    if (height > b.expires_at_height) return TxCode::ErrExpired;                          // T1
    if (b.expires_at_height - height > params.max_tx_ttl_blocks) return TxCode::ErrTtlTooLong;  // T2 (T1 => no underflow)
    const AccountState sender = state.account(b.sender()).value_or(AccountState{});
    if (b.nonce != sender.nonce) return TxCode::ErrNonce;                                // T3
    if (b.nonce == kU64Max) return TxCode::ErrOverflow;                                  // T4

    std::uint64_t amount = 0;
    if (const auto* t = std::get_if<tx::Transfer>(&b.payload)) {
        amount = t->amount;
    } else if (const auto* bn = std::get_if<tx::Burn>(&b.payload)) {
        amount = bn->amount;
    } else {
        return TxCode::ErrUnsupported;                                                   // T7 (until M3)
    }
    const auto total = checked_add(amount, b.fee);
    if (!total) return TxCode::ErrOverflow;                                              // T5
    if (sender.balance < *total) return TxCode::ErrInsufficientFunds;                    // T6
    return TxCode::Ok;
}

ValidationResult validate_tx(ByteView tx_bytes, std::uint64_t height, std::string_view chain_id,
                             const StateReader& state) {
    auto stateless = tx::check_stateless(tx_bytes, chain_id, state.params());
    ValidationResult out{stateless.code, std::move(stateless.tx)};
    if (out.code == TxCode::Ok) {
        out.code = check_stateful(*out.tx, height, state);
    }
    return out;
}

}  // namespace yap::state
