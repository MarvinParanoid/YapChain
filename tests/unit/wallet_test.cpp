// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <fstream>
#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "yap/common/checked.hpp"
#include "yap/wallet/amount.hpp"
#include "yap/wallet/keystore.hpp"

using namespace yap;
using namespace yap::wallet;
using nlohmann::json;

TEST_CASE("amount parsing is exact and strict", "[wallet][amount]") {
    CHECK(parse_amount("0") == 0U);
    CHECK(parse_amount("1500000") == 1'500'000U);
    CHECK(parse_amount("1.5YAP") == 1'500'000U);
    CHECK(parse_amount("1.5yap") == 1'500'000U);
    CHECK(parse_amount("2YAP") == 2'000'000U);
    CHECK(parse_amount("0.000001YAP") == 1U);
    CHECK(parse_amount("18446744073709551615") == kU64Max);
    CHECK(parse_amount("18446744073709.551615YAP") == kU64Max);
    for (const char* bad : {"", "-1", "+1", "1.5", "1e6", " 1", "1 ", "1.5 YAP", "0.0000001YAP", "1.YAP", ".5YAP",
                            "YAP", "18446744073709551616", "18446744073709.551616YAP", "1,5YAP", "0x10"}) {
        INFO(bad);
        CHECK_FALSE(parse_amount(bad).has_value());
    }
    CHECK(format_amount(1'500'001) == "1.500001 YAP (1500001 yip)");
    CHECK(format_amount(0) == "0.000000 YAP (0 yip)");
}

namespace {
json doc(const std::string& s) { return json::parse(s); }
}  // namespace

TEST_CASE("keystore round trip and failure modes", "[wallet][keystore]") {
    const auto k = create_keystore("correct horse", KeyPurpose::Account, KdfParams::interactive());
    auto info = read_key_info(k.json);
    REQUIRE(info.has_value());
    CHECK(info->address == k.info.address);
    CHECK(k.json.find("seed") == std::string::npos);

    auto key = unlock_keystore(k.json, "correct horse", KeyPurpose::Account);
    REQUIRE(key.has_value());
    CHECK(key->public_key() == k.info.public_key);

    CHECK(unlock_keystore(k.json, "wrong horse", KeyPurpose::Account).error() == KeystoreError::WrongPasswordOrCorrupted);
    CHECK(unlock_keystore(k.json, "correct horse", KeyPurpose::DevProducer).error() == KeystoreError::WrongPurpose);

    // Every header field is authenticated or validated.
    auto tamper = [&](const char* field, json value) {
        auto j = doc(k.json);
        j[field] = std::move(value);
        return unlock_keystore(j.dump(), "correct horse", KeyPurpose::Account);
    };
    auto flip_hex = [&](const char* field) {
        std::string h = doc(k.json)[field].get<std::string>();
        h[0] = h[0] == '0' ? '1' : '0';
        return h;
    };
    CHECK(tamper("ciphertext", flip_hex("ciphertext")).error() == KeystoreError::WrongPasswordOrCorrupted);
    CHECK(tamper("salt", flip_hex("salt")).error() == KeystoreError::WrongPasswordOrCorrupted);
    CHECK(tamper("nonce", flip_hex("nonce")).error() == KeystoreError::WrongPasswordOrCorrupted);
    CHECK(tamper("opslimit", "3").error() == KeystoreError::WrongPasswordOrCorrupted);  // in range, but bound by AAD
    CHECK(tamper("opslimit", "1").error() == KeystoreError::UnsupportedKdf);
    CHECK(tamper("memlimit", "8192").error() == KeystoreError::UnsupportedKdf);
    CHECK(tamper("purpose", "dev-producer").error() == KeystoreError::WrongPurpose);
    CHECK(tamper("version", 2).error() == KeystoreError::UnsupportedVersion);
    CHECK(tamper("kdf", "scrypt").error() == KeystoreError::UnsupportedKdf);
    CHECK(tamper("address", "tyap1qqq").error() == KeystoreError::Inconsistent);
    const auto other = create_keystore("x-password", KeyPurpose::Account, KdfParams::interactive());
    auto swapped = doc(k.json);
    swapped["public_key"] = doc(other.json)["public_key"];
    swapped["address"] = doc(other.json)["address"];
    CHECK(unlock_keystore(swapped.dump(), "correct horse", KeyPurpose::Account).error() ==
          KeystoreError::WrongPasswordOrCorrupted);

    auto extra = doc(k.json);
    extra["note"] = "hi";
    CHECK(read_key_info(extra.dump()).error() == KeystoreError::Malformed);
    auto dup = k.json;
    dup.insert(1, "\"kdf\": \"argon2id\",");
    CHECK(read_key_info(dup).error() == KeystoreError::Malformed);
    CHECK(read_key_info("not json").error() == KeystoreError::Malformed);
    CHECK(read_key_info(k.json.substr(0, k.json.size() / 2)).error() == KeystoreError::Malformed);
}

TEST_CASE("fresh keys are independent", "[wallet][keystore]") {
    const auto a = create_keystore("same password", KeyPurpose::Account, KdfParams::interactive());
    const auto b = create_keystore("same password", KeyPurpose::Account, KdfParams::interactive());
    CHECK(a.info.public_key != b.info.public_key);
    CHECK(doc(a.json)["salt"] != doc(b.json)["salt"]);
    CHECK(doc(a.json)["nonce"] != doc(b.json)["nonce"]);
}

TEST_CASE("private files: 0600, no overwrite, refuse open permissions", "[wallet][files]") {
    test::TempDir dir;
    const auto p = dir.path() / "keys" / "alice.json";
    write_private_file(p, "secret-ish");
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(p.parent_path().c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    CHECK(read_private_file(p) == "secret-ish");
    CHECK_THROWS(write_private_file(p, "other"));
    CHECK(read_private_file(p) == "secret-ish");
    std::filesystem::permissions(p, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    CHECK_THROWS(read_private_file(p));
    CHECK_THROWS(read_private_file(dir.path() / "missing.json"));
    CHECK(valid_key_name("alice_1-x"));
    CHECK_FALSE(valid_key_name("../alice"));
    CHECK_FALSE(valid_key_name("Alice"));
    CHECK_FALSE(valid_key_name(""));
}
