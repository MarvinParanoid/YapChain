// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/crypto/bech32m.hpp"

using namespace yap::crypto;

TEST_CASE("Bech32m encode/decode round trip", "[crypto][bech32m]") {
    const std::vector<std::uint8_t> data{0, 1, 2, 31, 30, 15};
    const auto s = bech32m_encode("test", data);
    auto d = bech32m_decode(s);
    REQUIRE(d.has_value());
    CHECK(d->hrp == "test");
    CHECK(d->data == data);
}

TEST_CASE("Bech32m decoding normalises uppercase", "[crypto][bech32m]") {
    const auto s = bech32m_encode("abc", {1, 2, 3});
    std::string upper;
    for (const char c : s) {
        upper.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    auto d = bech32m_decode(upper);
    REQUIRE(d.has_value());
    CHECK(d->hrp == "abc");
}

TEST_CASE("Bech32m detects every single-character substitution", "[crypto][bech32m]") {
    const std::string s = bech32m_encode("tyap", {0, 5, 10, 15, 20, 25, 30, 1, 2, 3});
    const std::string charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    for (std::size_t i = s.find('1') + 1; i < s.size(); ++i) {
        for (const char c : charset) {
            if (c == s[i]) {
                continue;
            }
            std::string t = s;
            t[i] = c;
            CHECK(bech32m_decode(t).error() == Bech32Error::InvalidChecksum);
        }
    }
}

TEST_CASE("Bech32m encoder rejects invalid input", "[crypto][bech32m]") {
    CHECK_THROWS_AS(bech32m_encode("", {}), std::invalid_argument);
    CHECK_THROWS_AS(bech32m_encode("ABC", {}), std::invalid_argument);
    CHECK_THROWS_AS(bech32m_encode("abc", {32}), std::invalid_argument);
    CHECK_THROWS_AS(bech32m_encode("abc", std::vector<std::uint8_t>(84, 0)), std::invalid_argument);
    CHECK_NOTHROW(bech32m_encode("abc", std::vector<std::uint8_t>(80, 0)));  // exactly 90 chars
}

TEST_CASE("convert_bits", "[crypto][bech32m]") {
    CHECK(convert_bits({0xff}, 8, 5, true) == std::vector<std::uint8_t>{31, 28});
    CHECK(convert_bits({31, 28}, 5, 8, false) == std::vector<std::uint8_t>{0xff});  // 2 zero padding bits
    CHECK(convert_bits({31, 24}, 5, 8, false) == std::vector<std::uint8_t>{0xfe});
    CHECK_FALSE(convert_bits({31, 29}, 5, 8, false).has_value());  // non-zero padding bits
    CHECK_FALSE(convert_bits({32}, 5, 8, false).has_value());  // input value out of range
    CHECK_FALSE(convert_bits({0, 0, 0}, 5, 8, false).has_value());  // 15 bits: 7 leftover >= 5
}
