// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "yap/common/bytes.hpp"

namespace yap {

/// Lowercase hexadecimal encoding.
[[nodiscard]] std::string to_hex(ByteView data);

/// Strict decoding: even length, lowercase [0-9a-f] only.
[[nodiscard]] std::optional<Bytes> from_hex(std::string_view text);

template <std::size_t N>
[[nodiscard]] std::optional<FixedBytes<N>> from_hex_fixed(std::string_view text) {
    if (text.size() != 2 * N) {
        return std::nullopt;
    }
    auto bytes = from_hex(text);
    if (!bytes) {
        return std::nullopt;
    }
    FixedBytes<N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = (*bytes)[i];
    }
    return out;
}

}  // namespace yap
