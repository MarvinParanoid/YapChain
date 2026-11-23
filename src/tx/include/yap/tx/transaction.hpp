// SPDX-License-Identifier: Apache-2.0
//
// Transaction and attestation formats (docs/protocol.md §6.1, attestations.md §3.1).
// Encoding is YCS; decoding is strict and enforces the field ceilings of §3.3.
#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "yap/codec/ycs.hpp"
#include "yap/common/bytes.hpp"
#include "yap/common/result.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/ed25519.hpp"

namespace yap::tx {

using crypto::AccountId;
using crypto::PublicKey;
using crypto::Signature;

inline constexpr std::uint16_t kTxVersion = 1;
inline constexpr std::uint16_t kAttestationVersion = 1;
/// Decoder ceiling for TxBody.memo (§3.3); the effective limit is ChainParams.max_memo_bytes.
inline constexpr std::uint32_t kMemoCeiling = 1024;

using ClaimId = FixedBytes<16>;

struct Attestation {
    std::uint16_t version = kAttestationVersion;
    std::string chain_id;
    std::uint32_t issuer_id = 0;
    std::uint8_t key_id = 0;
    std::uint32_t achievement_id = 0;
    AccountId subject{};
    ClaimId claim_id{};
    Hash32 evidence_hash{};
    std::uint64_t issued_at_s = 0;
    std::uint64_t expires_at_s = 0;

    bool operator==(const Attestation&) const = default;
    void encode_to(ycs::Writer& w) const;
    static Attestation decode_from(ycs::Reader& r);
};

struct SignedAttestation {
    Attestation attestation;
    Signature issuer_signature{};

    bool operator==(const SignedAttestation&) const = default;
    void encode_to(ycs::Writer& w) const;
    static SignedAttestation decode_from(ycs::Reader& r);
};

/// Signs "YAPCHAIN/ATTESTATION/V1\0" || YCS(attestation).
[[nodiscard]] SignedAttestation sign_attestation(const Attestation& attestation, const crypto::SecretKey& issuer_key);

enum class PayloadTag : std::uint8_t {
    Transfer = 0x00,
    Burn = 0x01,
    ClaimAchievement = 0x02,
};

struct Transfer {
    AccountId to{};
    std::uint64_t amount = 0;
    bool operator==(const Transfer&) const = default;
};

struct Burn {
    std::uint64_t amount = 0;
    bool operator==(const Burn&) const = default;
};

struct ClaimAchievement {
    SignedAttestation attestation;
    bool operator==(const ClaimAchievement&) const = default;
};

/// Alternative index == PayloadTag value.
using TxPayload = std::variant<Transfer, Burn, ClaimAchievement>;

[[nodiscard]] PayloadTag payload_tag(const TxPayload& payload) noexcept;

struct TxBody {
    std::uint16_t version = kTxVersion;
    std::string chain_id;
    PublicKey sender_pubkey{};
    std::uint64_t nonce = 0;
    std::uint64_t fee = 0;
    std::uint64_t expires_at_height = 0;
    Bytes memo;
    TxPayload payload = Transfer{};

    bool operator==(const TxBody&) const = default;
    void encode_to(ycs::Writer& w) const;
    static TxBody decode_from(ycs::Reader& r);

    /// account_id(sender_pubkey), docs/protocol.md §5.3.
    [[nodiscard]] AccountId sender() const;
};

struct SignedTx {
    TxBody body;
    Signature signature{};

    bool operator==(const SignedTx&) const = default;
    void encode_to(ycs::Writer& w) const;
    static SignedTx decode_from(ycs::Reader& r);
};

/// Signs "YAPCHAIN/TX/V1\0" || YCS(body). The body's sender_pubkey must match the key.
[[nodiscard]] SignedTx sign_tx(TxBody body, const crypto::SecretKey& sender_key);

/// tx_id = H_tag("YAP/txid/v1", tx_bytes).
[[nodiscard]] Hash32 tx_id(ByteView tx_bytes);

/// Strict decoding of tx_bytes = YCS(SignedTx).
[[nodiscard]] Result<SignedTx, ycs::DecodeError> decode_signed_tx(ByteView tx_bytes);

/// Helper used by decoders: chain_id string with the §2 content rule.
[[nodiscard]] std::string read_chain_id(ycs::Reader& r);

}  // namespace yap::tx
