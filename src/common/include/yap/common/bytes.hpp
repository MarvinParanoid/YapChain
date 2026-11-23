// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace yap {

static_assert(CHAR_BIT == 8, "YapChain requires 8-bit bytes");

using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ByteView = std::span<const Byte>;

template <std::size_t N>
using FixedBytes = std::array<Byte, N>;

using Hash32 = FixedBytes<32>;

/// View the bytes of a string without copying. `unsigned char` may alias any object.
[[nodiscard]] inline ByteView as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const Byte*>(s.data()), s.size()};
}

/// Concatenate byte sequences.
[[nodiscard]] inline Bytes concat(ByteView a, ByteView b) {
    Bytes out;
    out.reserve(a.size() + b.size());
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

}  // namespace yap
