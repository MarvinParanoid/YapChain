// SPDX-License-Identifier: Apache-2.0
#include "yap/tx/transaction.hpp"

#include <stdexcept>
#include <type_traits>

#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/tx/chain_id.hpp"

namespace yap::tx {

std::string read_chain_id(ycs::Reader& r) {
    std::string id = r.string(kMaxChainIdBytes);
    if (r.ok() && !is_valid_chain_id(id)) {
        r.fail(ycs::DecodeError::InvalidContent);
        return {};
    }
    return id;
}

// ------------------------------------------------------------------ attestation

void Attestation::encode_to(ycs::Writer& w) const {
    w.u16(version);
    w.string(chain_id);
    w.u32(issuer_id);
    w.u8(key_id);
    w.u32(achievement_id);
    w.fixed(subject);
    w.fixed(claim_id);
    w.fixed(evidence_hash);
    w.u64(issued_at_s);
    w.u64(expires_at_s);
}

Attestation Attestation::decode_from(ycs::Reader& r) {
    Attestation a;
    a.version = r.u16();
    a.chain_id = read_chain_id(r);
    a.issuer_id = r.u32();
    a.key_id = r.u8();
    a.achievement_id = r.u32();
    a.subject = r.fixed<32>();
    a.claim_id = r.fixed<16>();
    a.evidence_hash = r.fixed<32>();
    a.issued_at_s = r.u64();
    a.expires_at_s = r.u64();
    return a;
}

void SignedAttestation::encode_to(ycs::Writer& w) const {
    attestation.encode_to(w);
    w.fixed(issuer_signature);
}

SignedAttestation SignedAttestation::decode_from(ycs::Reader& r) {
    SignedAttestation s;
    s.attestation = Attestation::decode_from(r);
    s.issuer_signature = r.fixed<64>();
    return s;
}

SignedAttestation sign_attestation(const Attestation& attestation, const crypto::SecretKey& issuer_key) {
    return {attestation, issuer_key.sign_prefixed(crypto::sign_prefix::kAttestation, ycs::encode(attestation))};
}

// ------------------------------------------------------------------ transaction

PayloadTag payload_tag(const TxPayload& payload) noexcept { return static_cast<PayloadTag>(payload.index()); }

void TxBody::encode_to(ycs::Writer& w) const {
    w.u16(version);
    w.string(chain_id);
    w.fixed(sender_pubkey);
    w.u64(nonce);
    w.u64(fee);
    w.u64(expires_at_height);
    if (memo.size() > kMemoCeiling) {
        throw std::length_error("memo exceeds the YCS ceiling");
    }
    w.bytes(memo);
    w.enum_tag(static_cast<std::uint8_t>(payload_tag(payload)));
    std::visit(
        [&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Transfer>) {
                w.fixed(p.to);
                w.u64(p.amount);
            } else if constexpr (std::is_same_v<T, Burn>) {
                w.u64(p.amount);
            } else {
                p.attestation.encode_to(w);
            }
        },
        payload);
}

TxBody TxBody::decode_from(ycs::Reader& r) {
    TxBody b;
    b.version = r.u16();
    b.chain_id = read_chain_id(r);
    b.sender_pubkey = r.fixed<32>();
    b.nonce = r.u64();
    b.fee = r.u64();
    b.expires_at_height = r.u64();
    b.memo = r.bytes(kMemoCeiling);
    const std::uint8_t tag = r.enum_tag();
    if (!r.ok()) {
        return b;
    }
    switch (static_cast<PayloadTag>(tag)) {
        case PayloadTag::Transfer: {
            Transfer t;
            t.to = r.fixed<32>();
            t.amount = r.u64();
            b.payload = t;
            break;
        }
        case PayloadTag::Burn:
            b.payload = Burn{r.u64()};
            break;
        case PayloadTag::ClaimAchievement:
            b.payload = ClaimAchievement{SignedAttestation::decode_from(r)};
            break;
        default:
            // 0x03..0xFF: reserved or not defined in this protocol version (§6.1).
            r.fail(ycs::DecodeError::InvalidEnumTag);
            break;
    }
    return b;
}

AccountId TxBody::sender() const { return crypto::account_id_from_public_key(sender_pubkey); }

void SignedTx::encode_to(ycs::Writer& w) const {
    body.encode_to(w);
    w.fixed(signature);
}

SignedTx SignedTx::decode_from(ycs::Reader& r) {
    SignedTx tx;
    tx.body = TxBody::decode_from(r);
    tx.signature = r.fixed<64>();
    return tx;
}

SignedTx sign_tx(TxBody body, const crypto::SecretKey& sender_key) {
    if (body.sender_pubkey != sender_key.public_key()) {
        throw std::invalid_argument("sign_tx: sender_pubkey does not match the signing key");
    }
    const Signature sig = sender_key.sign_prefixed(crypto::sign_prefix::kTx, ycs::encode(body));
    return SignedTx{std::move(body), sig};
}

Hash32 tx_id(ByteView tx_bytes) { return crypto::tagged_hash(crypto::tag::kTxId, tx_bytes); }

Result<SignedTx, ycs::DecodeError> decode_signed_tx(ByteView tx_bytes) { return ycs::decode<SignedTx>(tx_bytes); }

}  // namespace yap::tx
