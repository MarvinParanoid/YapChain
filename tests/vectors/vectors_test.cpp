// SPDX-License-Identifier: Apache-2.0
//
// Checks the C++ implementation against test-vectors/*.json produced by the
// independent Python generator in tools/refgen (no shared code, no libsodium).
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "sample.hpp"
#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/bech32m.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/ed25519.hpp"
#include "yap/crypto/hash.hpp"

using namespace yap;
using json = nlohmann::json;

namespace {

json load(const std::string& name) {
    std::ifstream in(std::string(YAP_TEST_VECTORS_DIR) + "/" + name);
    if (!in) {
        throw std::runtime_error("cannot open test vector file " + name);
    }
    json doc = json::parse(in);
    REQUIRE(doc.at("version") == 1);
    return doc.at("vectors");
}

Bytes hex(const json& j) {
    auto b = from_hex(j.get<std::string>());
    if (!b) {
        throw std::runtime_error("bad hex in vectors");
    }
    return *b;
}

template <std::size_t N>
FixedBytes<N> hexn(const json& j) {
    auto b = from_hex_fixed<N>(j.get<std::string>());
    if (!b) {
        throw std::runtime_error("bad fixed hex in vectors");
    }
    return *b;
}

std::uint64_t u64(const json& j) { return std::stoull(j.get<std::string>()); }

std::string str(ByteView b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

}  // namespace

TEST_CASE("sha256 vectors", "[vectors][sha256]") {
    for (const auto& v : load("sha256.json")) {
        INFO(v.at("name").get<std::string>());
        Bytes msg;
        if (v.contains("repeat_count")) {
            msg.assign(v.at("repeat_count").get<std::size_t>(), hex(v.at("repeat_byte")).at(0));
        } else {
            msg = hex(v.at("msg"));
        }
        CHECK(to_hex(crypto::sha256(msg)) == v.at("digest").get<std::string>());
    }
}

TEST_CASE("tagged hash vectors and signature prefixes", "[vectors][tagged]") {
    const auto vec = load("tagged_hash.json");
    std::size_t registry_hits = 0;
    for (const auto& v : vec.at("hashes")) {
        const auto tag = v.at("tag").get<std::string>();
        CHECK(to_hex(crypto::tagged_hash(tag, hex(v.at("msg")))) == v.at("digest").get<std::string>());
        for (const auto t : crypto::tag::kAll) {
            registry_hits += (t == tag) ? 1U : 0U;
        }
    }
    CHECK(registry_hits == crypto::tag::kAll.size() * 3);  // every registry tag is covered

    for (const auto& p : vec.at("sign_prefixes")) {
        const auto name = p.at("name").get<std::string>();
        const auto expected = str(hex(p.at("bytes")));
        if (name == "tx") {
            CHECK(crypto::sign_prefix::kTx == expected);
        } else if (name == "attestation") {
            CHECK(crypto::sign_prefix::kAttestation == expected);
        } else if (name == "devblock") {
            CHECK(crypto::sign_prefix::kDevBlock == expected);
        } else {
            FAIL("unknown prefix " << name);
        }
    }
}

TEST_CASE("checked arithmetic vectors", "[vectors][arith]") {
    const auto vec = load("arith.json");
    auto expect = [](const json& v, std::optional<std::uint64_t> got) {
        if (v.at("r").is_null()) {
            CHECK_FALSE(got.has_value());
        } else {
            REQUIRE(got.has_value());
            CHECK(*got == u64(v.at("r")));
        }
    };
    for (const auto& v : vec.at("checked_add")) expect(v, checked_add(u64(v.at("a")), u64(v.at("b"))));
    for (const auto& v : vec.at("checked_sub")) expect(v, checked_sub(u64(v.at("a")), u64(v.at("b"))));
    for (const auto& v : vec.at("checked_mul")) expect(v, checked_mul(u64(v.at("a")), u64(v.at("b"))));
    for (const auto& v : vec.at("mul_div_floor"))
        expect(v, mul_div_floor(u64(v.at("a")), u64(v.at("b")), u64(v.at("c"))));
}

// ---------------------------------------------------------------- YCS

namespace {

struct Decoded {
    std::optional<ycs::DecodeError> error;
    json value;
    Bytes reencoded;
};

json opt_u64(const std::optional<std::uint64_t>& v) { return v ? json(std::to_string(*v)) : json(nullptr); }

Decoded decode_schema(const std::string& schema, ByteView in) {
    ycs::Reader r(in);
    ycs::Writer w;
    json value;
    if (schema == "u8") {
        const auto x = r.u8();
        w.u8(x);
        value = std::to_string(x);
    } else if (schema == "u16") {
        const auto x = r.u16();
        w.u16(x);
        value = std::to_string(x);
    } else if (schema == "u32") {
        const auto x = r.u32();
        w.u32(x);
        value = std::to_string(x);
    } else if (schema == "u64") {
        const auto x = r.u64();
        w.u64(x);
        value = std::to_string(x);
    } else if (schema == "bool") {
        const auto x = r.boolean();
        w.boolean(x);
        value = x;
    } else if (schema == "fixed4") {
        const auto x = r.fixed<4>();
        w.fixed(x);
        value = to_hex(x);
    } else if (schema == "bytes8") {
        const auto x = r.bytes(8);
        w.bytes(x);
        value = to_hex(x);
    } else if (schema == "ascii8") {
        const auto x = r.ascii_printable(8);
        w.string(x);
        value = x;
    } else if (schema == "option_u32") {
        std::optional<std::uint64_t> x;
        if (r.option_tag()) {
            x = r.u32();
        }
        w.option_tag(x.has_value());
        if (x) {
            w.u32(static_cast<std::uint32_t>(*x));
        }
        value = opt_u64(x);
    } else if (schema == "vec_u16_3") {
        const auto n = r.vec_length(3, 2);
        value = json::array();
        std::vector<std::uint16_t> xs;
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            xs.push_back(r.u16());
            value.push_back(std::to_string(xs.back()));
        }
        w.length(xs.size());
        for (const auto x : xs) w.u16(x);
    } else if (schema == "sample") {
        const auto s = test::Sample::decode_from(r);
        s.encode_to(w);
        value = {{"version", std::to_string(s.version)},
                 {"name", s.name},
                 {"tag", to_hex(s.tag)},
                 {"amount", std::to_string(s.amount)},
                 {"maybe", opt_u64(s.maybe)},
                 {"flag", s.flag}};
        value["items"] = json::array();
        for (const auto& it : s.items) value["items"].push_back(to_hex(it));
        value["kind"] = s.kind_value ? json{{"tag", 1}, {"v", std::to_string(*s.kind_value)}} : json{{"tag", 0}};
    } else {
        throw std::runtime_error("unknown schema " + schema);
    }
    Decoded d;
    auto st = r.finish();
    if (!st) {
        d.error = st.error();
    }
    d.value = value;
    d.reencoded = std::move(w).take();
    return d;
}

}  // namespace

TEST_CASE("YCS vectors", "[vectors][ycs]") {
    const auto vec = load("ycs.json");
    for (const auto& v : vec.at("valid")) {
        const auto schema = v.at("schema").get<std::string>();
        const Bytes in = hex(v.at("hex"));
        INFO(schema << " " << v.at("hex").get<std::string>());
        const auto d = decode_schema(schema, in);
        REQUIRE_FALSE(d.error.has_value());
        CHECK(d.value == v.at("value"));
        CHECK(d.reencoded == in);  // canonical: encode(decode(b)) == b
    }
    for (const auto& v : vec.at("invalid")) {
        const auto schema = v.at("schema").get<std::string>();
        INFO(schema << " " << v.at("hex").get<std::string>());
        const auto d = decode_schema(schema, hex(v.at("hex")));
        REQUIRE(d.error.has_value());
        CHECK(ycs::to_string(*d.error) == v.at("error").get<std::string>());
    }
}

// ---------------------------------------------------------------- Ed25519

TEST_CASE("Ed25519 RFC 8032 and deterministic vectors", "[vectors][ed25519]") {
    const auto vec = load("ed25519.json");
    for (const auto& group : {"rfc8032", "deterministic"}) {
        for (const auto& v : vec.at(group)) {
            const auto key = crypto::SecretKey::from_seed(hexn<32>(v.at("seed")));
            CHECK(to_hex(key.public_key()) == v.at("pub").get<std::string>());
            const Bytes msg = hex(v.at("msg"));
            const auto sig = key.sign(msg);
            CHECK(to_hex(sig) == v.at("sig").get<std::string>());
            CHECK(crypto::verify(key.public_key(), msg, sig));
        }
    }
}

TEST_CASE("Ed25519 verification edge cases match protocol §5.2", "[vectors][ed25519]") {
    const auto vec = load("ed25519.json");
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (const auto& v : vec.at("verify")) {
        INFO(v.at("name").get<std::string>() << ": " << v.at("note").get<std::string>());
        const bool expected = v.at("valid").get<bool>();
        const bool got = crypto::verify(hexn<32>(v.at("pub")), hex(v.at("msg")), hexn<64>(v.at("sig")));
        CHECK(got == expected);
        (expected ? accepted : rejected) += 1;
    }
    CHECK(accepted >= 2);   // baseline + mixed-order key with k = 0 mod 8
    CHECK(rejected >= 15);
}

TEST_CASE("validator public key vectors match protocol §5.7", "[vectors][ed25519][validator]") {
    const auto vec = load("ed25519.json");
    std::size_t valid = 0;
    std::size_t invalid = 0;
    for (const auto& v : vec.at("validator_keys")) {
        INFO(v.at("name").get<std::string>() << ": " << v.at("note").get<std::string>());
        const bool expected = v.at("valid").get<bool>();
        CHECK(crypto::is_valid_validator_public_key(hexn<32>(v.at("pub"))) == expected);
        (expected ? valid : invalid) += 1;
    }
    CHECK(valid >= 5);
    CHECK(invalid >= 14);

    // ADR-0020 tightens only validator keys: a mixed-order key still verifies user signatures.
    bool checked_mixed = false;
    for (const auto& v : vec.at("verify")) {
        if (v.at("name") == "mixed_order_a_k_div_8") {
            const auto pub = hexn<32>(v.at("pub"));
            CHECK(crypto::verify(pub, hex(v.at("msg")), hexn<64>(v.at("sig"))));
            CHECK_FALSE(crypto::is_valid_validator_public_key(pub));
            checked_mixed = true;
        }
    }
    CHECK(checked_mixed);
}

// ---------------------------------------------------------------- Bech32m and addresses

TEST_CASE("Bech32m BIP-350 vectors", "[vectors][bech32m]") {
    const auto vec = load("bech32m.json");
    for (const auto& v : vec.at("valid")) {
        const std::string text = str(hex(v.at("input")));
        INFO(text);
        auto d = crypto::bech32m_decode(text);
        REQUIRE(d.has_value());
        CHECK(d->hrp == v.at("hrp").get<std::string>());
        CHECK(d->data == v.at("data").get<std::vector<std::uint8_t>>());
        CHECK(crypto::bech32m_encode(d->hrp, d->data) == v.at("canonical").get<std::string>());
    }
    for (const auto& v : vec.at("invalid")) {
        const std::string text = str(hex(v.at("input")));
        INFO(v.at("input").get<std::string>());
        auto d = crypto::bech32m_decode(text);
        REQUIRE_FALSE(d.has_value());
        CHECK(crypto::to_string(d.error()) == v.at("error").get<std::string>());
    }
}

TEST_CASE("address vectors", "[vectors][address]") {
    const auto vec = load("address.json");
    const auto hrp = vec.at("hrp").get<std::string>();
    for (const auto& v : vec.at("keys")) {
        const auto key = crypto::SecretKey::from_seed(hexn<32>(v.at("seed")));
        CHECK(to_hex(key.public_key()) == v.at("pub").get<std::string>());
        const auto id = crypto::account_id_from_public_key(key.public_key());
        CHECK(to_hex(id) == v.at("account_id").get<std::string>());
        CHECK(crypto::encode_address(hrp, id) == v.at("address").get<std::string>());
    }
    for (const auto& v : vec.at("system_accounts")) {
        const auto id = crypto::system_account_id(v.at("name").get<std::string>());
        CHECK(to_hex(id) == v.at("account_id").get<std::string>());
        CHECK(crypto::encode_address(hrp, id) == v.at("address").get<std::string>());
    }
    for (const auto& v : vec.at("decode")) {
        INFO(v.at("name").get<std::string>());
        auto r = crypto::decode_address(str(hex(v.at("input"))), v.at("expected_hrp").get<std::string>());
        if (v.at("ok").get<bool>()) {
            REQUIRE(r.has_value());
            CHECK(to_hex(*r) == v.at("account_id").get<std::string>());
        } else {
            REQUIRE_FALSE(r.has_value());
            CHECK(crypto::to_string(r.error()) == v.at("error").get<std::string>());
        }
    }
}
