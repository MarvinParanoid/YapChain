// SPDX-License-Identifier: Apache-2.0
//
// Checked unsigned 64-bit arithmetic (docs/protocol.md §1, ADR-0017).
// Every operation either returns the exact mathematical result or std::nullopt;
// there is no wrap-around and no saturation.
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace yap {

inline constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (a > kU64Max - b) {
        return std::nullopt;
    }
    return a + b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_sub(std::uint64_t a, std::uint64_t b) noexcept {
    if (b > a) {
        return std::nullopt;
    }
    return a - b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
    if (a != 0 && b > kU64Max / a) {
        return std::nullopt;
    }
    return a * b;
}

namespace detail {
__extension__ using U128 = unsigned __int128;  // GCC/Clang extension; both supported compilers provide it
}  // namespace detail

/// floor(a * b / c) computed exactly with a 128-bit intermediate product.
/// Returns nullopt if c == 0 or the quotient does not fit in 64 bits.
[[nodiscard]] constexpr std::optional<std::uint64_t> mul_div_floor(std::uint64_t a, std::uint64_t b,
                                                                   std::uint64_t c) noexcept {
    if (c == 0) {
        return std::nullopt;
    }
    const detail::U128 q = static_cast<detail::U128>(a) * static_cast<detail::U128>(b) / c;
    if (q > static_cast<detail::U128>(kU64Max)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(q);
}

}  // namespace yap
