// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "yap/crypto/address.hpp"
#include "yap/crypto/bech32m.hpp"

using namespace yap;
using namespace yap::crypto;

TEST_CASE("address round trip and length", "[crypto][address]") {
    Seed seed{};
    seed.fill(9);
    const auto key = SecretKey::from_seed(seed);
    const auto id = account_id_from_public_key(key.public_key());
    const auto addr = encode_address(kTestnetHrp, id);
    CHECK(addr.size() == 64);
    CHECK(addr.starts_with("tyap1q"));  // version 0 encodes as 'q'
    auto back = decode_address(addr, kTestnetHrp);
    REQUIRE(back.has_value());
    CHECK(*back == id);
    CHECK(decode_address(addr, kMainnetHrp).error() == AddressError::WrongHrp);
}

TEST_CASE("system accounts are distinct and not key-derived", "[crypto][address]") {
    const auto treasury = system_account_id(kTreasuryAccountName);
    const auto pool = system_account_id(kRewardsPoolAccountName);
    CHECK(treasury != pool);
    CHECK(decode_address(encode_address(kTestnetHrp, treasury), kTestnetHrp).value() == treasury);
}

TEST_CASE("address decoding rejects wrong shapes", "[crypto][address]") {
    AccountId id{};
    id.fill(0xaa);
    std::vector<std::uint8_t> five = *convert_bits(std::vector<std::uint8_t>(id.begin(), id.end()), 8, 5, true);

    auto with = [&](std::vector<std::uint8_t> data) { return decode_address(bech32m_encode("tyap", data), "tyap"); };
    std::vector<std::uint8_t> v1{1};
    v1.insert(v1.end(), five.begin(), five.end());
    CHECK(with(v1).error() == AddressError::WrongVersion);

    std::vector<std::uint8_t> shortv{0};
    shortv.insert(shortv.end(), five.begin(), five.end() - 1);
    CHECK(with(shortv).error() == AddressError::WrongLength);

    std::vector<std::uint8_t> pad{0};
    pad.insert(pad.end(), five.begin(), five.end());
    pad.back() |= 1;
    CHECK(with(pad).error() == AddressError::NonZeroPadding);

    CHECK(with({}).error() == AddressError::WrongLength);
    CHECK(decode_address("not an address", "tyap").error() == AddressError::Bech32);
}
