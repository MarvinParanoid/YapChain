// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"
#include "yap/common/result.hpp"

using namespace yap;

TEST_CASE("checked_add boundaries", "[common][checked]") {
    CHECK(checked_add(0, 0) == 0U);
    CHECK(checked_add(kU64Max, 0) == kU64Max);
    CHECK(checked_add(kU64Max - 1, 1) == kU64Max);
    CHECK_FALSE(checked_add(kU64Max, 1).has_value());
    CHECK_FALSE(checked_add(1, kU64Max).has_value());
    CHECK_FALSE(checked_add(kU64Max, kU64Max).has_value());
}

TEST_CASE("checked_sub boundaries", "[common][checked]") {
    CHECK(checked_sub(5, 5) == 0U);
    CHECK(checked_sub(kU64Max, kU64Max) == 0U);
    CHECK_FALSE(checked_sub(0, 1).has_value());
    CHECK_FALSE(checked_sub(4, 5).has_value());
}

TEST_CASE("checked_mul boundaries", "[common][checked]") {
    CHECK(checked_mul(0, kU64Max) == 0U);
    CHECK(checked_mul(kU64Max, 1) == kU64Max);
    CHECK(checked_mul(1ULL << 32, (1ULL << 32) - 1) == (1ULL << 32) * ((1ULL << 32) - 1));
    CHECK_FALSE(checked_mul(1ULL << 32, 1ULL << 32).has_value());
    CHECK_FALSE(checked_mul(kU64Max, 2).has_value());
}

TEST_CASE("mul_div_floor uses a 128-bit product", "[common][checked]") {
    CHECK(mul_div_floor(kU64Max, kU64Max, kU64Max) == kU64Max);
    CHECK(mul_div_floor(kU64Max, 5000, 10000) == kU64Max / 2);
    CHECK(mul_div_floor(1'200'000, 10, 30) == 400'000U);
    CHECK(mul_div_floor(7, 3, 2) == 10U);
    CHECK_FALSE(mul_div_floor(1, 1, 0).has_value());
    CHECK_FALSE(mul_div_floor(kU64Max, 3, 2).has_value());
    static_assert(mul_div_floor(2'000'000, 6000, 10000) == 1'200'000U);
}

TEST_CASE("hex is strict lowercase", "[common][hex]") {
    const Bytes b{0x00, 0x0f, 0xa0, 0xff};
    CHECK(to_hex(b) == "000fa0ff");
    CHECK(from_hex("000fa0ff") == b);
    CHECK(from_hex("")->empty());
    CHECK_FALSE(from_hex("0").has_value());
    CHECK_FALSE(from_hex("0G").has_value());
    CHECK_FALSE(from_hex("AB").has_value());
    CHECK_FALSE(from_hex(" 0").has_value());
    CHECK(from_hex_fixed<2>("abcd") == FixedBytes<2>{0xab, 0xcd});
    CHECK_FALSE(from_hex_fixed<2>("abcdef").has_value());
}

TEST_CASE("Result distinguishes value and error", "[common][result]") {
    Result<int, std::string> ok = 5;
    Result<int, std::string> err = unexpected(std::string("boom"));
    CHECK(ok.has_value());
    CHECK(*ok == 5);
    CHECK_THROWS_AS(ok.error(), BadResultAccess);
    CHECK_FALSE(err.has_value());
    CHECK(err.error() == "boom");
    CHECK_THROWS_AS(err.value(), BadResultAccess);
    Status<int> st = Ok{};
    CHECK(st.has_value());
}
