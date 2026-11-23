// SPDX-License-Identifier: Apache-2.0
//
// State value types (docs/protocol.md §7.2) and raw state keys (§7.1).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "yap/codec/ycs.hpp"
#include "yap/crypto/address.hpp"
#include "yap/state/account.hpp"

namespace yap::state {

using ConsensusAddress = FixedBytes<20>;

namespace ns {
inline constexpr Byte kChainState = 0x00;
inline constexpr Byte kAccount = 0x01;
inline constexpr Byte kParams = 0x02;
inline constexpr Byte kValidator = 0x03;
inline constexpr Byte kIssuer = 0x04;
inline constexpr Byte kIssuerUsage = 0x05;
inline constexpr Byte kAchievement = 0x06;
inline constexpr Byte kClaimCounter = 0x07;
inline constexpr Byte kSubjectUsage = 0x08;
inline constexpr Byte kNullifier = 0x09;
}  // namespace ns

struct ChainState {
    std::uint64_t height = 0;
    std::uint64_t last_time_ms = 0;
    std::uint64_t total_supply = 0;
    std::uint64_t total_minted = 0;
    std::uint64_t total_burned = 0;

    bool operator==(const ChainState&) const = default;
    void encode_to(ycs::Writer& w) const;
    static ChainState decode_from(ycs::Reader& r);
};

inline constexpr std::uint32_t kMaxNameBytes = 32;

struct ValidatorInfo {
    std::string name;
    crypto::PublicKey consensus_pubkey{};
    std::uint64_t power = 0;
    crypto::AccountId reward_account{};

    bool operator==(const ValidatorInfo&) const = default;
    void encode_to(ycs::Writer& w) const;
    static ValidatorInfo decode_from(ycs::Reader& r);
};

/// attestations.md §3.2. Status values: 1 = active, 2 = suspended/revoked/disabled.
inline constexpr std::uint8_t kStatusActive = 1;
inline constexpr std::uint8_t kStatusInactive = 2;
inline constexpr std::uint32_t kMaxIssuerKeys = 8;

struct IssuerKey {
    std::uint8_t key_id = 0;
    crypto::PublicKey pubkey{};
    std::uint8_t status = kStatusActive;
    bool operator==(const IssuerKey&) const = default;
};

struct Issuer {
    std::uint32_t issuer_id = 0;
    std::string name;
    std::uint8_t status = kStatusActive;
    std::uint64_t epoch_budget = 0;
    std::vector<IssuerKey> keys;

    bool operator==(const Issuer&) const = default;
    void encode_to(ycs::Writer& w) const;
    static Issuer decode_from(ycs::Reader& r);
};

inline constexpr std::uint8_t kPolicyOnce = 1;
inline constexpr std::uint8_t kPolicyPerEpoch = 2;

struct Achievement {
    std::uint32_t issuer_id = 0;
    std::uint32_t achievement_id = 0;
    std::uint64_t reward = 0;
    std::uint8_t policy = kPolicyOnce;
    std::uint16_t max_per_epoch = 0;
    std::uint8_t status = kStatusActive;

    bool operator==(const Achievement&) const = default;
    void encode_to(ycs::Writer& w) const;
    static Achievement decode_from(ycs::Reader& r);
};

/// CometBFT consensus address: first 20 bytes of SHA-256(pubkey) (protocol.md §5.5).
[[nodiscard]] ConsensusAddress consensus_address(const crypto::PublicKey& pubkey);

namespace key {
[[nodiscard]] Bytes chain_state();
[[nodiscard]] Bytes params();
[[nodiscard]] Bytes account(const crypto::AccountId& id);
[[nodiscard]] Bytes validator(const ConsensusAddress& address);
[[nodiscard]] Bytes issuer(std::uint32_t issuer_id);
[[nodiscard]] Bytes achievement(std::uint32_t issuer_id, std::uint32_t achievement_id);
}  // namespace key

}  // namespace yap::state
