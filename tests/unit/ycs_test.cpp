// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "sample.hpp"
#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"

using namespace yap;
using ycs::DecodeError;

namespace {
Bytes hex(std::string_view s) { return *from_hex(s); }
}  // namespace

TEST_CASE("YCS integers are little-endian fixed width", "[ycs]") {
    ycs::Writer w;
    w.u8(0xab);
    w.u16(0x0102);
    w.u32(0x01020304);
    w.u64(0x0102030405060708ULL);
    CHECK(to_hex(w.data()) == "ab" "0201" "04030201" "0807060504030201");

    ycs::Reader r(w.data());
    CHECK(r.u8() == 0xab);
    CHECK(r.u16() == 0x0102);
    CHECK(r.u32() == 0x01020304U);
    CHECK(r.u64() == 0x0102030405060708ULL);
    CHECK(r.finish().has_value());
}

TEST_CASE("YCS bytes, strings, options", "[ycs]") {
    ycs::Writer w;
    w.bytes(Bytes{1, 2, 3});
    w.string("yap");
    w.option_tag(false);
    w.option_tag(true);
    w.u32(7);
    CHECK(to_hex(w.data()) == "03000000010203" "03000000796170" "00" "01" "07000000");
}

TEST_CASE("YCS sticky error: first error wins and later reads are no-ops", "[ycs]") {
    const Bytes in = hex("02ffff");
    ycs::Reader r(in);
    CHECK_FALSE(r.boolean());
    CHECK(r.error() == DecodeError::InvalidBool);
    CHECK(r.u16() == 0);  // no-op after error
    CHECK(r.remaining() == 2);
    CHECK(r.finish().error() == DecodeError::InvalidBool);
}

TEST_CASE("YCS rejects trailing bytes", "[ycs]") {
    const Bytes in = hex("0100");
    ycs::Reader r(in);
    (void)r.u8();
    CHECK(r.finish().error() == DecodeError::TrailingBytes);
}

TEST_CASE("YCS length limit is checked before allocation", "[ycs]") {
    // Declares 4 GiB of bytes; must fail without trying to allocate.
    const Bytes in = hex("ffffffff");
    ycs::Reader r(in);
    CHECK(r.bytes(1u << 20).empty());
    CHECK(r.error() == DecodeError::LengthLimit);

    // Within the limit but longer than the input: Truncated, still no allocation.
    ycs::Reader r2(in);
    CHECK(r2.bytes(0xffffffffu).empty());
    CHECK(r2.error() == DecodeError::Truncated);
}

TEST_CASE("YCS vec_length bounds the count by remaining bytes", "[ycs]") {
    const Bytes in = hex("ffffff0f" "0000");  // count 268M of u16, only 2 bytes left
    ycs::Reader r(in);
    CHECK(r.vec_length(0xffffffffu, 2) == 0);
    CHECK(r.error() == DecodeError::Truncated);
}

TEST_CASE("YCS ascii_printable rejects control and non-ASCII bytes", "[ycs]") {
    for (const char* h : {"0100000009", "010000007f", "02000000c3a9"}) {
        const Bytes in = hex(h);
        ycs::Reader r(in);
        (void)r.ascii_printable(8);
        CHECK(r.error() == DecodeError::InvalidContent);
    }
}

TEST_CASE("YCS sample round trip and canonicality", "[ycs]") {
    test::Sample s;
    s.version = 1;
    s.name = "memes";
    s.tag = {1, 2, 3, 4};
    s.amount = kU64Max;
    s.maybe = 42;
    s.items = {Bytes{}, Bytes{0xaa}, Bytes{1, 2, 3, 4}};
    s.flag = true;
    s.kind_value = 9;
    const Bytes enc = ycs::encode(s);
    auto dec = ycs::decode<test::Sample>(enc);
    REQUIRE(dec.has_value());
    CHECK(*dec == s);
    CHECK(ycs::encode(*dec) == enc);

    Bytes bad = enc;
    bad[bad.size() - 5] = 0x02;  // enum tag precedes the trailing u32 of Kind::Value
    CHECK(ycs::decode<test::Sample>(bad).error() == DecodeError::InvalidEnumTag);
}

TEST_CASE("YCS writer rejects lengths above u32", "[ycs]") {
    ycs::Writer w;
    CHECK_THROWS_AS(w.length(std::size_t{1} << 32), std::length_error);
}
