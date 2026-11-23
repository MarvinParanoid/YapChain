// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/crypto/domains.hpp"
#include "yap/crypto/ed25519.hpp"

using namespace yap;
using namespace yap::crypto;

namespace {
Seed seed_of(Byte b) {
    Seed s{};
    s.fill(b);
    return s;
}
}  // namespace

TEST_CASE("Ed25519 signing is deterministic", "[crypto][ed25519]") {
    const auto k1 = SecretKey::from_seed(seed_of(7));
    const auto k2 = SecretKey::from_seed(seed_of(7));
    CHECK(k1.public_key() == k2.public_key());
    CHECK(k1.sign(as_bytes("msg")) == k2.sign(as_bytes("msg")));
    CHECK(k1.sign(as_bytes("msg")) != k1.sign(as_bytes("msh")));
}

TEST_CASE("Ed25519 sign/verify and tamper detection", "[crypto][ed25519]") {
    const auto key = SecretKey::from_seed(seed_of(1));
    const auto other = SecretKey::from_seed(seed_of(2));
    const auto sig = key.sign(as_bytes("yap"));
    CHECK(verify(key.public_key(), as_bytes("yap"), sig));
    CHECK_FALSE(verify(key.public_key(), as_bytes("yap!"), sig));
    CHECK_FALSE(verify(other.public_key(), as_bytes("yap"), sig));
    for (std::size_t i = 0; i < sig.size(); ++i) {
        auto bad = sig;
        bad[i] ^= 0x01;
        CHECK_FALSE(verify(key.public_key(), as_bytes("yap"), bad));
    }
}

TEST_CASE("domain prefixes separate signature types", "[crypto][ed25519]") {
    const auto key = SecretKey::from_seed(seed_of(3));
    const Bytes payload{1, 2, 3};
    const auto sig = key.sign_prefixed(sign_prefix::kTx, payload);
    CHECK(verify_prefixed(key.public_key(), sign_prefix::kTx, payload, sig));
    CHECK_FALSE(verify_prefixed(key.public_key(), sign_prefix::kAttestation, payload, sig));
    CHECK_FALSE(verify_prefixed(key.public_key(), sign_prefix::kDevBlock, payload, sig));
    CHECK_FALSE(verify(key.public_key(), payload, sig));
}

TEST_CASE("generated keys are distinct and usable", "[crypto][ed25519]") {
    const auto a = SecretKey::generate();
    const auto b = SecretKey::generate();
    CHECK(a.public_key() != b.public_key());
    CHECK(verify(a.public_key(), as_bytes("x"), a.sign(as_bytes("x"))));
}

TEST_CASE("SecretKey move semantics", "[crypto][ed25519]") {
    auto a = SecretKey::from_seed(seed_of(4));
    const auto pk = a.public_key();
    SecretKey b = std::move(a);
    CHECK(b.public_key() == pk);
    CHECK_THROWS_AS(a.sign(as_bytes("x")), std::logic_error);  // NOLINT(bugprone-use-after-move)
    a = std::move(b);
    CHECK(verify(pk, as_bytes("x"), a.sign(as_bytes("x"))));
}
