// SPDX-License-Identifier: Apache-2.0
#include "yap/common/hex.hpp"

// ADR-0030: the project is C++23 (GCC 13 reports 202100L in C++23 mode, every later supported
// compiler 202302L); a build that fell back to C++20 (202002L) stops here.
static_assert(__cplusplus > 202002L, "YapChain requires C++23 (ADR-0030)");

namespace yap {
namespace {

constexpr char kDigits[] = "0123456789abcdef";

constexpr int nibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

std::string to_hex(ByteView data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (const Byte b : data) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

std::optional<Bytes> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]);
        const int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<Byte>((hi << 4) | lo));
    }
    return out;
}

}  // namespace yap
