// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

/// Order of byte keys in every ordered container: unsigned lexicographic, a proper prefix first —
/// the order of LMDB and of std::less<Bytes>, on which iteration order and state roots depend.
/// Written with an explicit memcmp because GCC 14 at -O3 reports a false -Wstringop-overread inside
/// the inlined std::vector<unsigned char> comparison. Transparent: lookups by ByteView need no copy.
/// memcmp is never called with a zero length, so empty views with a null data() are fine.
struct BytesLess {
    using is_transparent = void;
    [[nodiscard]] bool operator()(ByteView a, ByteView b) const noexcept {
        const std::size_t n = std::min(a.size(), b.size());
        if (n != 0) {
            if (const int c = std::memcmp(a.data(), b.data(), n); c != 0) return c < 0;
        }
        return a.size() < b.size();
    }
};

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
