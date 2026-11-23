// SPDX-License-Identifier: Apache-2.0
//
// YapChain account identifiers and addresses (docs/protocol.md §5.3, §5.4).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "yap/common/bytes.hpp"
#include "yap/common/result.hpp"
#include "yap/crypto/ed25519.hpp"

namespace yap::crypto {

using AccountId = FixedBytes<32>;

inline constexpr std::string_view kTestnetHrp = "tyap";
inline constexpr std::string_view kMainnetHrp = "yap";
inline constexpr std::uint8_t kAddressVersion = 0;
inline constexpr std::uint8_t kAlgorithmEd25519 = 0x00;

/// account_id = H_tag("YAP/addr/v1", 0x00 || pubkey)
[[nodiscard]] AccountId account_id_from_public_key(const PublicKey& public_key);

/// account_id = H_tag("YAP/sys-account/v1", name)
[[nodiscard]] AccountId system_account_id(std::string_view name);

inline constexpr std::string_view kTreasuryAccountName = "treasury";
inline constexpr std::string_view kRewardsPoolAccountName = "rewards-pool";

/// Canonical (lowercase) Bech32m address.
[[nodiscard]] std::string encode_address(std::string_view hrp, const AccountId& account_id);

enum class AddressError : std::uint8_t {
    Bech32 = 1,      ///< not a valid Bech32m string
    WrongHrp,
    WrongVersion,
    WrongLength,
    NonZeroPadding,
};

[[nodiscard]] std::string_view to_string(AddressError e) noexcept;

[[nodiscard]] Result<AccountId, AddressError> decode_address(std::string_view address, std::string_view expected_hrp);

}  // namespace yap::crypto
