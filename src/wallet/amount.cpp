// SPDX-License-Identifier: Apache-2.0
#include "yap/wallet/amount.hpp"

#include "yap/common/checked.hpp"

namespace yap::wallet {
namespace {

std::optional<std::uint64_t> parse_digits(std::string_view s) {
    if (s.empty() || s.size() > 20) return std::nullopt;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto m = checked_mul(v, 10);
        if (!m) return std::nullopt;
        const auto a = checked_add(*m, static_cast<std::uint64_t>(c - '0'));
        if (!a) return std::nullopt;
        v = *a;
    }
    return v;
}

bool ends_with_yap(std::string_view s) {
    if (s.size() < 3) return false;
    const auto t = s.substr(s.size() - 3);
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    return lower(t[0]) == 'y' && lower(t[1]) == 'a' && lower(t[2]) == 'p';
}

}  // namespace

std::optional<std::uint64_t> parse_decimal_u64(std::string_view text) {
    if (text.size() > 1 && text[0] == '0') return std::nullopt;  // "010" must not mean anything
    return parse_digits(text);
}

std::optional<std::uint64_t> parse_amount(std::string_view text) {
    if (!ends_with_yap(text)) return parse_decimal_u64(text);  // plain integer: yip
    const auto number = text.substr(0, text.size() - 3);
    const auto dot = number.find('.');
    const auto whole = parse_decimal_u64(number.substr(0, dot));
    if (!whole) return std::nullopt;
    std::uint64_t frac = 0;
    if (dot != std::string_view::npos) {
        const auto f = number.substr(dot + 1);
        if (f.empty() || f.size() > 6) return std::nullopt;
        const auto digits = parse_digits(f);
        if (!digits) return std::nullopt;
        frac = *digits;
        for (std::size_t i = f.size(); i < 6; ++i) frac *= 10;  // at most 999999, no overflow
    }
    const auto w = checked_mul(*whole, kYipPerYap);
    if (!w) return std::nullopt;
    return checked_add(*w, frac);
}

std::string format_amount(std::uint64_t yip) {
    std::string frac = std::to_string(yip % kYipPerYap);
    frac.insert(0, 6 - frac.size(), '0');
    return std::to_string(yip / kYipPerYap) + "." + frac + " YAP (" + std::to_string(yip) + " yip)";
}

}  // namespace yap::wallet
