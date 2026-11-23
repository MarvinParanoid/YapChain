// SPDX-License-Identifier: Apache-2.0
#include "yap/tx/codes.hpp"

namespace yap::tx {

std::string_view to_string(TxCode code) noexcept {
    switch (code) {
        case TxCode::Ok: return "OK";
        case TxCode::ErrDecode: return "ERR_DECODE";
        case TxCode::ErrTooLarge: return "ERR_TOO_LARGE";
        case TxCode::ErrVersion: return "ERR_VERSION";
        case TxCode::ErrChainId: return "ERR_CHAIN_ID";
        case TxCode::ErrSignature: return "ERR_SIGNATURE";
        case TxCode::ErrFee: return "ERR_FEE";
        case TxCode::ErrMemo: return "ERR_MEMO";
        case TxCode::ErrExpired: return "ERR_EXPIRED";
        case TxCode::ErrTtlTooLong: return "ERR_TTL_TOO_LONG";
        case TxCode::ErrNonce: return "ERR_NONCE";
        case TxCode::ErrInsufficientFunds: return "ERR_INSUFFICIENT_FUNDS";
        case TxCode::ErrPayload: return "ERR_PAYLOAD";
        case TxCode::ErrOverflow: return "ERR_OVERFLOW";
        case TxCode::ErrUnsupported: return "ERR_UNSUPPORTED";
        case TxCode::ErrAttestation: return "ERR_ATTESTATION";
        case TxCode::ErrClaimIssuer: return "ERR_CLAIM_ISSUER";
        case TxCode::ErrClaimAchievement: return "ERR_CLAIM_ACHIEVEMENT";
        case TxCode::ErrClaimDuplicate: return "ERR_CLAIM_DUPLICATE";
        case TxCode::ErrClaimLimit: return "ERR_CLAIM_LIMIT";
        case TxCode::ErrClaimBudget: return "ERR_CLAIM_BUDGET";
        case TxCode::ErrClaimPoolEmpty: return "ERR_CLAIM_POOL_EMPTY";
        case TxCode::ErrClaimTime: return "ERR_CLAIM_TIME";
    }
    return "UNKNOWN";
}

}  // namespace yap::tx
