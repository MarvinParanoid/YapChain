// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace yap::tx {

inline constexpr std::uint32_t kMaxChainIdBytes = 32;

/// docs/protocol.md §2: ASCII [a-z0-9-], 1..32 bytes, no leading or trailing '-'.
[[nodiscard]] constexpr bool is_valid_chain_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxChainIdBytes || id.front() == '-' || id.back() == '-') {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace yap::tx
