// SPDX-License-Identifier: Apache-2.0
#include "yap/tx/stateless.hpp"

#include "yap/crypto/domains.hpp"

namespace yap::tx {

bool attestation_stateless_ok(const TxBody& body, const SignedAttestation& claim, const ChainParams& params) {
    const Attestation& a = claim.attestation;
    if (a.version != kAttestationVersion) return false;
    if (a.chain_id != body.chain_id) return false;
    if (a.subject != body.sender()) return false;
    if (a.issued_at_s >= a.expires_at_s) return false;
    // Subtraction is safe: issued_at_s < expires_at_s.
    return a.expires_at_s - a.issued_at_s <= params.max_attestation_validity_s;
}

StatelessResult check_stateless(ByteView tx_bytes, std::string_view chain_id, const ChainParams& params) {
    StatelessResult out;
    // S1 — before decoding, on the raw bytes.
    if (tx_bytes.size() > params.max_tx_bytes) {
        out.code = TxCode::ErrTooLarge;
        return out;
    }
    // S2
    auto decoded = decode_signed_tx(tx_bytes);
    if (!decoded) {
        out.code = TxCode::ErrDecode;
        return out;
    }
    out.tx = std::move(decoded).value();
    const TxBody& b = out.tx->body;

    auto fail = [&out](TxCode c) {
        out.code = c;
        return out;
    };
    if (b.version != kTxVersion) return fail(TxCode::ErrVersion);                // S3
    if (b.chain_id != chain_id) return fail(TxCode::ErrChainId);                 // S4
    if (b.memo.size() > params.max_memo_bytes) return fail(TxCode::ErrMemo);     // S5
    if (b.fee < params.min_fee) return fail(TxCode::ErrFee);                     // S6
    if (b.expires_at_height < 1) return fail(TxCode::ErrExpired);                // S7
    if (const auto* t = std::get_if<Transfer>(&b.payload)) {                     // S8
        if (t->amount < 1 || t->to == b.sender()) return fail(TxCode::ErrPayload);
    }
    if (const auto* bn = std::get_if<Burn>(&b.payload)) {                        // S9
        if (bn->amount < 1) return fail(TxCode::ErrPayload);
    }
    if (const auto* c = std::get_if<ClaimAchievement>(&b.payload)) {             // S10
        if (!attestation_stateless_ok(b, c->attestation, params)) return fail(TxCode::ErrAttestation);
    }
    if (!crypto::verify_prefixed(b.sender_pubkey, crypto::sign_prefix::kTx, ycs::encode(b),   // S11
                                 out.tx->signature)) {
        return fail(TxCode::ErrSignature);
    }
    out.code = TxCode::Ok;
    return out;
}

}  // namespace yap::tx
