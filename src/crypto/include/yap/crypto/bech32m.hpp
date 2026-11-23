// SPDX-License-Identifier: Apache-2.0
//
// Bech32m (BIP-350). Bech32 (BIP-173) checksums are rejected.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "yap/common/result.hpp"

namespace yap::crypto {

enum class Bech32Error : std::uint8_t {
    InvalidLength = 1,   ///< longer than 90 characters
    InvalidCharacter,    ///< outside 0x21..0x7E, or data character not in the alphabet
    MixedCase,
    MissingSeparator,
    InvalidHrp,          ///< empty HRP
    DataTooShort,        ///< fewer than 6 data characters
    InvalidChecksum,
};

[[nodiscard]] std::string_view to_string(Bech32Error e) noexcept;

struct Bech32mDecoded {
    std::string hrp;                 ///< lowercase
    std::vector<std::uint8_t> data;  ///< 5-bit values, checksum removed
};

/// Throws std::invalid_argument if the HRP is empty/invalid/not lowercase, any value
/// exceeds 31 or the result would exceed 90 characters.
[[nodiscard]] std::string bech32m_encode(std::string_view hrp, const std::vector<std::uint8_t>& data);

[[nodiscard]] Result<Bech32mDecoded, Bech32Error> bech32m_decode(std::string_view text);

/// General power-of-two base conversion (BIP-173 `convertbits`).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> convert_bits(const std::vector<std::uint8_t>& in,
                                                                     unsigned from_bits, unsigned to_bits,
                                                                     bool pad);

}  // namespace yap::crypto
