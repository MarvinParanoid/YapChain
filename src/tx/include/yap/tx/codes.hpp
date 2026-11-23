// SPDX-License-Identifier: Apache-2.0
//
// Transaction result codes (docs/protocol.md §13). Values are consensus-critical
// (they enter CometBFT's last_results_hash) and must never be renumbered.
#pragma once

#include <cstdint>
#include <string_view>

namespace yap::tx {

enum class TxCode : std::uint32_t {
    Ok = 0,
    ErrDecode = 1,
    ErrTooLarge = 2,
    ErrVersion = 3,
    ErrChainId = 4,
    ErrSignature = 5,
    ErrFee = 6,
    ErrMemo = 7,
    ErrExpired = 8,
    ErrTtlTooLong = 9,
    ErrNonce = 10,
    ErrInsufficientFunds = 11,
    ErrPayload = 12,
    ErrOverflow = 13,
    ErrUnsupported = 14,
    ErrAttestation = 20,
    ErrClaimIssuer = 21,
    ErrClaimAchievement = 22,
    ErrClaimDuplicate = 23,
    ErrClaimLimit = 24,
    ErrClaimBudget = 25,
    ErrClaimPoolEmpty = 26,
    ErrClaimTime = 27,
};

/// Protocol name of the code, e.g. "ERR_NONCE".
[[nodiscard]] std::string_view to_string(TxCode code) noexcept;

}  // namespace yap::tx
