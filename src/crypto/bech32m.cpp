// SPDX-License-Identifier: Apache-2.0
#include "yap/crypto/bech32m.hpp"

#include <array>
#include <stdexcept>

namespace yap::crypto {
namespace {

constexpr std::string_view kCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr std::uint32_t kBech32mConst = 0x2bc830a3;
constexpr std::size_t kMaxLength = 90;
constexpr std::size_t kChecksumLength = 6;

constexpr std::array<std::int8_t, 128> make_reverse_charset() {
    std::array<std::int8_t, 128> rev{};
    for (auto& v : rev) {
        v = -1;
    }
    for (std::size_t i = 0; i < kCharset.size(); ++i) {
        rev[static_cast<unsigned char>(kCharset[i])] = static_cast<std::int8_t>(i);
    }
    return rev;
}
constexpr auto kReverse = make_reverse_charset();

std::uint32_t polymod(const std::vector<std::uint8_t>& values) {
    constexpr std::array<std::uint32_t, 5> kGen{0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    std::uint32_t chk = 1;
    for (const std::uint8_t v : values) {
        const std::uint32_t top = chk >> 25;
        chk = ((chk & 0x1ffffffU) << 5) ^ v;
        for (std::size_t i = 0; i < kGen.size(); ++i) {
            if (((top >> i) & 1U) != 0) {
                chk ^= kGen[i];
            }
        }
    }
    return chk;
}

std::vector<std::uint8_t> hrp_expand(std::string_view hrp) {
    std::vector<std::uint8_t> out;
    out.reserve(hrp.size() * 2 + 1);
    for (const char c : hrp) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c) >> 5));
    }
    out.push_back(0);
    for (const char c : hrp) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c) & 31U));
    }
    return out;
}

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

std::string_view to_string(Bech32Error e) noexcept {
    switch (e) {
        case Bech32Error::InvalidLength: return "invalid_length";
        case Bech32Error::InvalidCharacter: return "invalid_character";
        case Bech32Error::MixedCase: return "mixed_case";
        case Bech32Error::MissingSeparator: return "missing_separator";
        case Bech32Error::InvalidHrp: return "invalid_hrp";
        case Bech32Error::DataTooShort: return "data_too_short";
        case Bech32Error::InvalidChecksum: return "invalid_checksum";
    }
    return "unknown";
}

std::string bech32m_encode(std::string_view hrp, const std::vector<std::uint8_t>& data) {
    if (hrp.empty()) {
        throw std::invalid_argument("bech32m: empty HRP");
    }
    for (const char c : hrp) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 33 || b > 126 || (c >= 'A' && c <= 'Z')) {
            throw std::invalid_argument("bech32m: HRP must be lowercase printable ASCII");
        }
    }
    if (hrp.size() + 1 + data.size() + kChecksumLength > kMaxLength) {
        throw std::invalid_argument("bech32m: result longer than 90 characters");
    }
    std::vector<std::uint8_t> values = hrp_expand(hrp);
    for (const std::uint8_t d : data) {
        if (d > 31) {
            throw std::invalid_argument("bech32m: data value above 31");
        }
        values.push_back(d);
    }
    values.insert(values.end(), kChecksumLength, 0);
    const std::uint32_t mod = polymod(values) ^ kBech32mConst;

    std::string out(hrp);
    out.push_back('1');
    for (const std::uint8_t d : data) {
        out.push_back(kCharset[d]);
    }
    for (std::size_t i = 0; i < kChecksumLength; ++i) {
        out.push_back(kCharset[(mod >> (5 * (5 - i))) & 31U]);
    }
    return out;
}

Result<Bech32mDecoded, Bech32Error> bech32m_decode(std::string_view text) {
    if (text.size() > kMaxLength) {
        return unexpected(Bech32Error::InvalidLength);
    }
    bool has_lower = false;
    bool has_upper = false;
    for (const char c : text) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 33 || b > 126) {
            return unexpected(Bech32Error::InvalidCharacter);
        }
        has_lower = has_lower || (c >= 'a' && c <= 'z');
        has_upper = has_upper || (c >= 'A' && c <= 'Z');
    }
    if (has_lower && has_upper) {
        return unexpected(Bech32Error::MixedCase);
    }
    const std::size_t pos = text.rfind('1');
    if (pos == std::string_view::npos) {
        return unexpected(Bech32Error::MissingSeparator);
    }
    if (pos == 0) {
        return unexpected(Bech32Error::InvalidHrp);
    }
    if (text.size() - pos - 1 < kChecksumLength) {
        return unexpected(Bech32Error::DataTooShort);
    }

    Bech32mDecoded out;
    out.hrp.reserve(pos);
    for (std::size_t i = 0; i < pos; ++i) {
        out.hrp.push_back(to_lower(text[i]));
    }
    std::vector<std::uint8_t> data;
    data.reserve(text.size() - pos - 1);
    for (std::size_t i = pos + 1; i < text.size(); ++i) {
        const auto b = static_cast<unsigned char>(to_lower(text[i]));
        const std::int8_t v = kReverse[b];  // b <= 126 checked above
        if (v < 0) {
            return unexpected(Bech32Error::InvalidCharacter);
        }
        data.push_back(static_cast<std::uint8_t>(v));
    }

    std::vector<std::uint8_t> values = hrp_expand(out.hrp);
    values.insert(values.end(), data.begin(), data.end());
    if (polymod(values) != kBech32mConst) {
        return unexpected(Bech32Error::InvalidChecksum);
    }
    data.resize(data.size() - kChecksumLength);
    out.data = std::move(data);
    return out;
}

std::optional<std::vector<std::uint8_t>> convert_bits(const std::vector<std::uint8_t>& in, unsigned from_bits,
                                                      unsigned to_bits, bool pad) {
    if (from_bits == 0 || from_bits > 8 || to_bits == 0 || to_bits > 8) {
        throw std::invalid_argument("convert_bits: widths must be 1..8");
    }
    std::uint32_t acc = 0;
    unsigned bits = 0;
    const std::uint32_t max_value = (1U << to_bits) - 1;
    const std::uint32_t max_acc = (1U << (from_bits + to_bits - 1)) - 1;
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * from_bits / to_bits + 1);
    for (const std::uint8_t v : in) {
        if ((static_cast<std::uint32_t>(v) >> from_bits) != 0) {
            return std::nullopt;
        }
        acc = ((acc << from_bits) | v) & max_acc;
        bits += from_bits;
        while (bits >= to_bits) {
            bits -= to_bits;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & max_value));
        }
    }
    if (pad) {
        if (bits != 0) {
            out.push_back(static_cast<std::uint8_t>((acc << (to_bits - bits)) & max_value));
        }
    } else if (bits >= from_bits || ((acc << (to_bits - bits)) & max_value) != 0) {
        return std::nullopt;
    }
    return out;
}

}  // namespace yap::crypto
