// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "fake_state.hpp"
#include "vector_util.hpp"

using namespace yap;
using test::jhex;
using test::jhexn;
using test::ju64;

TEST_CASE("validation vectors: S1..S11, T1..T7 in normative order", "[vectors][validation]") {
    const auto vec = test::load_vectors("validation.json");
    test::FakeState st;
    const auto& p = vec.at("params");
    st.p.min_fee = ju64(p.at("min_fee"));
    st.p.fee_burn_bps = static_cast<std::uint16_t>(ju64(p.at("fee_burn_bps")));
    st.p.max_tx_bytes = static_cast<std::uint32_t>(ju64(p.at("max_tx_bytes")));
    st.p.max_memo_bytes = static_cast<std::uint32_t>(ju64(p.at("max_memo_bytes")));
    st.p.max_tx_ttl_blocks = ju64(p.at("max_tx_ttl_blocks"));
    st.p.max_attestation_validity_s = ju64(p.at("max_attestation_validity_s"));
    REQUIRE(st.p == tx::testnet_params());  // vectors use exactly the testnet parameters
    for (const auto& a : vec.at("accounts")) {
        st.accounts[jhexn<32>(a.at("account_id"))] = {ju64(a.at("balance")), ju64(a.at("nonce"))};
    }
    const auto height = ju64(vec.at("height"));
    const auto chain_id = vec.at("chain_id").get<std::string>();

    std::size_t n = 0;
    for (const auto& c : vec.at("cases")) {
        INFO(c.at("name").get<std::string>());
        const auto r = state::validate_tx(jhex(c.at("tx_hex")), height, chain_id, st);
        CHECK(tx::to_string(r.code) == c.at("code").get<std::string>());
        ++n;
    }
    CHECK(n >= 40);
}
