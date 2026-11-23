// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/crypto/domains.hpp"
#include "yap/tx/chain_id.hpp"
#include "yap/tx/transaction.hpp"

using namespace yap;

namespace {
crypto::SecretKey key(Byte b) {
    crypto::Seed s{};
    s.fill(b);
    return crypto::SecretKey::from_seed(s);
}

tx::TxBody transfer_body(const crypto::SecretKey& k) {
    tx::TxBody b;
    b.chain_id = "yapchain-testnet-1";
    b.sender_pubkey = k.public_key();
    b.nonce = 5;
    b.fee = 1000;
    b.expires_at_height = 10;
    b.memo = {0x6d, 0x65, 0x6d};
    b.payload = tx::Transfer{crypto::system_account_id("treasury"), 42};
    return b;
}
}  // namespace

TEST_CASE("chain_id rules (protocol §2)", "[tx]") {
    CHECK(tx::is_valid_chain_id("yapchain-testnet-1"));
    CHECK(tx::is_valid_chain_id("a"));
    CHECK(tx::is_valid_chain_id(std::string(32, 'z')));
    CHECK_FALSE(tx::is_valid_chain_id(""));
    CHECK_FALSE(tx::is_valid_chain_id(std::string(33, 'z')));
    CHECK_FALSE(tx::is_valid_chain_id("-yap"));
    CHECK_FALSE(tx::is_valid_chain_id("yap-"));
    CHECK_FALSE(tx::is_valid_chain_id("Yap"));
    CHECK_FALSE(tx::is_valid_chain_id("yap_chain"));
    CHECK_FALSE(tx::is_valid_chain_id("yap chain"));
}

TEST_CASE("signed transaction round trip for every payload", "[tx]") {
    const auto k = key(1);
    auto body = transfer_body(k);
    std::vector<tx::TxPayload> payloads{tx::Transfer{{}, 1}, tx::Burn{9}};
    tx::Attestation att;
    att.chain_id = "yapchain-testnet-1";
    att.issuer_id = 3;
    att.subject = body.sender();
    payloads.emplace_back(tx::ClaimAchievement{tx::sign_attestation(att, key(2))});
    for (const auto& p : payloads) {
        body.payload = p;
        const auto stx = tx::sign_tx(body, k);
        const Bytes bytes = ycs::encode(stx);
        auto back = tx::decode_signed_tx(bytes);
        REQUIRE(back.has_value());
        CHECK(*back == stx);
        CHECK(ycs::encode(*back) == bytes);
        CHECK(tx::payload_tag(back->body.payload) == tx::payload_tag(p));
    }
}

TEST_CASE("signature covers the whole body", "[tx]") {
    const auto k = key(3);
    const auto stx = tx::sign_tx(transfer_body(k), k);
    const Bytes body_bytes = ycs::encode(stx.body);
    CHECK(crypto::verify_prefixed(k.public_key(), crypto::sign_prefix::kTx, body_bytes, stx.signature));
    auto changed = stx.body;
    changed.nonce += 1;
    CHECK_FALSE(crypto::verify_prefixed(k.public_key(), crypto::sign_prefix::kTx, ycs::encode(changed), stx.signature));
}

TEST_CASE("sign_tx refuses a mismatching key", "[tx]") {
    CHECK_THROWS_AS(tx::sign_tx(transfer_body(key(4)), key(5)), std::invalid_argument);
}

TEST_CASE("tx_id depends on every byte, including the signature", "[tx]") {
    const auto k = key(6);
    Bytes bytes = ycs::encode(tx::sign_tx(transfer_body(k), k));
    const auto id = tx::tx_id(bytes);
    bytes.back() ^= 1;
    CHECK(tx::tx_id(bytes) != id);
}

TEST_CASE("decoder enforces ceilings before allocation", "[tx]") {
    const auto k = key(7);
    auto body = transfer_body(k);
    body.memo.assign(tx::kMemoCeiling, 0xAB);
    CHECK(tx::decode_signed_tx(ycs::encode(tx::sign_tx(body, k))).has_value());
    body.memo.push_back(0);
    CHECK_THROWS_AS(ycs::encode(body), std::length_error);  // the encoder never emits invalid memo sizes

    // A declared memo length of 2^32-1 is rejected as LengthLimit without allocating.
    Bytes evil = ycs::encode(transfer_body(k));
    const std::size_t memo_len_pos = 2 + 4 + 18 + 32 + 8 + 8 + 8;
    evil[memo_len_pos] = evil[memo_len_pos + 1] = evil[memo_len_pos + 2] = evil[memo_len_pos + 3] = 0xFF;
    CHECK(tx::decode_signed_tx(evil).error() == ycs::DecodeError::LengthLimit);
}

TEST_CASE("signing refuses bodies the strict decoder would reject", "[tx][audit]") {
    const auto k = key(8);
    auto body = transfer_body(k);
    body.chain_id = "Bad Id";
    CHECK_THROWS_AS(tx::sign_tx(body, k), std::invalid_argument);
    tx::Attestation att;
    att.chain_id = "UPPER";
    CHECK_THROWS_AS(tx::sign_attestation(att, k), std::invalid_argument);
}
