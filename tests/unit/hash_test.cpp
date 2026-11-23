// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/common/hex.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"

using namespace yap;
using namespace yap::crypto;

TEST_CASE("SHA-256 known answers", "[crypto][sha256]") {
    CHECK(to_hex(sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(to_hex(sha256(as_bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("incremental SHA-256 equals one-shot", "[crypto][sha256]") {
    Bytes data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<Byte>(i * 7);
    }
    Sha256 h;
    h.update(ByteView(data).first(1)).update(ByteView(data).subspan(1, 499)).update(ByteView(data).subspan(500));
    CHECK(h.finalize() == sha256(data));
    CHECK_THROWS_AS(h.update(data), std::logic_error);
}

TEST_CASE("tagged hash is length-prefixed", "[crypto][tagged]") {
    // H_tag("ab", "c") != H_tag("a", "bc") although tag||msg is identical.
    CHECK(tagged_hash("ab", as_bytes("c")) != tagged_hash("a", as_bytes("bc")));
    Bytes manual{2, 'a', 'b', 'c'};
    CHECK(tagged_hash("ab", as_bytes("c")) == sha256(manual));
}

TEST_CASE("tagged hash rejects invalid tags", "[crypto][tagged]") {
    CHECK_THROWS_AS(tagged_hash("", {}), std::invalid_argument);
    CHECK_THROWS_AS(tagged_hash(std::string(256, 'a'), {}), std::invalid_argument);
    CHECK_THROWS_AS(tagged_hash("has space", {}), std::invalid_argument);
    CHECK_NOTHROW(tagged_hash(std::string(255, 'a'), {}));
}

TEST_CASE("signature prefixes are prefix-free and NUL-terminated", "[crypto][domains]") {
    const std::string_view all[] = {sign_prefix::kTx, sign_prefix::kAttestation, sign_prefix::kDevBlock};
    for (const auto a : all) {
        CHECK(a.back() == '\0');
        CHECK(a.find('\0') == a.size() - 1);
        for (const auto b : all) {
            if (a != b) {
                CHECK_FALSE(b.starts_with(a));
            }
        }
    }
    CHECK(sign_prefix::kTx.size() == 15);
}
