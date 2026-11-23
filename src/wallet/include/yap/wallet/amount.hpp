// SPDX-License-Identifier: Apache-2.0
//
// Exact YAP amounts without floating point. 1 YAP = 1 000 000 yip (economics.md §2).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace yap::wallet {

inline constexpr std::uint64_t kYipPerYap = 1'000'000;

/// Accepted forms (no spaces, no signs, no exponents, no leading zeros):
///   "1500000"      -> 1 500 000 yip (a plain integer is always yip)
///   "1.5YAP", "2yap", "0.000001YAP" -> decimal YAP with at most 6 fractional digits
/// Returns nullopt on any syntax error or u64 overflow.
[[nodiscard]] std::optional<std::uint64_t> parse_amount(std::string_view text);

/// Strict decimal u64: "0" or [1-9][0-9]* (no signs, prefixes, separators, leading zeros), no overflow.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal_u64(std::string_view text);

/// "1.500000 YAP (1500000 yip)"
[[nodiscard]] std::string format_amount(std::uint64_t yip);

}  // namespace yap::wallet
