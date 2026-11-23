// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "vector_util.hpp"
#include "yap/tx/transaction.hpp"

using namespace yap;
using test::jhex;
using test::jhexn;
using test::json;
using test::ju64;

namespace {

tx::Attestation attestation_from(const json& a) {
    tx::Attestation att;
    att.version = a.at("version").get<std::uint16_t>();
    att.chain_id = a.at("chain_id").get<std::string>();
    att.issuer_id = a.at("issuer_id").get<std::uint32_t>();
    att.key_id = a.at("key_id").get<std::uint8_t>();
    att.achievement_id = a.at("achievement_id").get<std::uint32_t>();
    att.subject = jhexn<32>(a.at("subject"));
    att.claim_id = jhexn<16>(a.at("claim_id"));
    att.evidence_hash = jhexn<32>(a.at("evidence_hash"));
    att.issued_at_s = ju64(a.at("issued_at_s"));
    att.expires_at_s = ju64(a.at("expires_at_s"));
    return att;
}

tx::TxBody body_from(const json& j, const crypto::PublicKey& sender) {
    tx::TxBody b;
    b.version = j.at("version").get<std::uint16_t>();
    b.chain_id = j.at("chain_id").get<std::string>();
    b.sender_pubkey = sender;
    b.nonce = ju64(j.at("nonce"));
    b.fee = ju64(j.at("fee"));
    b.expires_at_height = ju64(j.at("expires_at_height"));
    b.memo = jhex(j.at("memo"));
    const auto& p = j.at("payload");
    const auto type = p.at("type").get<std::string>();
    if (type == "transfer") {
        b.payload = tx::Transfer{jhexn<32>(p.at("to")), ju64(p.at("amount"))};
    } else if (type == "burn") {
        b.payload = tx::Burn{ju64(p.at("amount"))};
    } else {
        b.payload = tx::ClaimAchievement{
            tx::SignedAttestation{attestation_from(p.at("attestation")), jhexn<64>(p.at("issuer_signature"))}};
    }
    return b;
}

}  // namespace

TEST_CASE("transaction encoding vectors", "[vectors][tx]") {
    const auto vec = test::load_vectors("tx.json");
    for (const auto& v : vec.at("encode")) {
        INFO(v.at("name").get<std::string>());
        const auto key = crypto::SecretKey::from_seed(jhexn<32>(v.at("seed")));
        const auto body = body_from(v.at("body"), key.public_key());

        CHECK(to_hex(ycs::encode(body)) == v.at("body_hex").get<std::string>());
        CHECK(to_hex(body.sender()) == v.at("sender_account_id").get<std::string>());

        const auto signed_tx = tx::sign_tx(body, key);
        const Bytes tx_bytes = ycs::encode(signed_tx);
        CHECK(to_hex(signed_tx.signature) == v.at("signature").get<std::string>());
        CHECK(to_hex(tx_bytes) == v.at("tx_hex").get<std::string>());
        CHECK(to_hex(tx::tx_id(tx_bytes)) == v.at("tx_id").get<std::string>());

        auto decoded = tx::decode_signed_tx(jhex(v.at("tx_hex")));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == signed_tx);

        if (v.contains("issuer_seed")) {
            const auto issuer = crypto::SecretKey::from_seed(jhexn<32>(v.at("issuer_seed")));
            const auto& claim = std::get<tx::ClaimAchievement>(body.payload);
            CHECK(tx::sign_attestation(claim.attestation.attestation, issuer) == claim.attestation);
        }
    }
}

TEST_CASE("transaction strict decoding vectors", "[vectors][tx]") {
    const auto vec = test::load_vectors("tx.json");
    for (const auto& v : vec.at("decode_invalid")) {
        INFO(v.at("name").get<std::string>());
        auto decoded = tx::decode_signed_tx(jhex(v.at("hex")));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(ycs::to_string(decoded.error()) == v.at("error").get<std::string>());
    }
}
