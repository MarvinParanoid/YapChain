// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger_fixture.hpp"
#include "vector_util.hpp"
#include "yap/state/economics.hpp"

using namespace yap;
using test::jhexn;
using test::ju64;

namespace {
tx::ChainParams params_from(const test::json& p) {
    tx::ChainParams c;
    c.min_fee = ju64(p.at("min_fee"));
    c.fee_burn_bps = static_cast<std::uint16_t>(ju64(p.at("fee_burn_bps")));
    c.block_reward = ju64(p.at("block_reward"));
    c.reward_validators_bps = static_cast<std::uint16_t>(ju64(p.at("reward_validators_bps")));
    c.reward_pool_bps = static_cast<std::uint16_t>(ju64(p.at("reward_pool_bps")));
    c.reward_treasury_bps = static_cast<std::uint16_t>(ju64(p.at("reward_treasury_bps")));
    c.max_issuance = ju64(p.at("max_issuance"));
    c.max_tx_bytes = static_cast<std::uint32_t>(ju64(p.at("max_tx_bytes")));
    c.max_memo_bytes = static_cast<std::uint32_t>(ju64(p.at("max_memo_bytes")));
    c.max_tx_ttl_blocks = ju64(p.at("max_tx_ttl_blocks"));
    c.epoch_length_blocks = ju64(p.at("epoch_length_blocks"));
    c.max_attestation_validity_s = ju64(p.at("max_attestation_validity_s"));
    c.attestation_clock_skew_s = ju64(p.at("attestation_clock_skew_s"));
    c.max_claims_per_subject_per_epoch = static_cast<std::uint16_t>(ju64(p.at("max_claims_per_subject_per_epoch")));
    return c;
}
}  // namespace

TEST_CASE("emission vectors (protocol §9.5)", "[vectors][economics]") {
    const auto vec = test::load_vectors("economics.json");
    for (const auto& c : vec.at("cases")) {
        INFO(c.at("name").get<std::string>());
        const auto params = params_from(c.at("params"));
        REQUIRE(tx::check_params(params).empty());

        state::MemoryKvStore store;
        state::Overlay setup(&store);
        state::State st(setup);
        st.put_params(params);
        const std::uint64_t minted = ju64(c.at("total_minted_before"));
        // A holder account keeps the supply invariant consistent with total_minted.
        const crypto::AccountId holder = test::id_of(test::key_of(0xEE));
        if (minted > 0) st.put_account(holder, {minted, 0});
        st.put_chain_state({1, 0, minted, minted, 0});
        for (const auto& v : c.at("validators")) {
            const auto pub = jhexn<32>(v.at("consensus_pubkey"));
            const auto addr = state::consensus_address(pub);
            REQUIRE(to_hex(addr) == v.at("address").get<std::string>());
            st.put_validator(addr, {"v", pub, ju64(v.at("power")), jhexn<32>(v.at("reward_account"))});
        }
        setup.apply_to(store);

        std::vector<state::CommitVote> commit;
        for (const auto& v : c.at("last_commit")) {
            commit.push_back({jhexn<20>(v.at("address")), ju64(v.at("power")), v.at("flag").get<std::uint8_t>()});
        }
        state::Overlay block(&store);
        state::State bst(block);
        const auto before = test::dump(block);
        state::validate_last_commit(bst, commit.empty() ? 1 : 2, commit);
        std::vector<state::Event> events;
        CHECK(state::apply_block_reward(bst, commit, events) == ju64(c.at("minted")));

        // Exactly the expected accounts were credited; no zero-balance accounts appeared.
        std::map<Bytes, Bytes> expected = before;
        for (const auto& cr : c.at("credits")) {
            const auto id = jhexn<32>(cr.at("account_id"));
            const auto old = state::State(block).account(id);  // after; compare amounts below
            CHECK(old.has_value());
            CHECK(old->balance == ju64(cr.at("amount")));
            expected[state::key::account(id)] = ycs::encode(*old);
        }
        if (ju64(c.at("minted")) == 0) {
            CHECK(test::dump(block) == before);  // §9.5: reward 0 leaves the state untouched
        } else {
            const auto cs = bst.chain_state();
            CHECK(cs.total_minted == minted + ju64(c.at("minted")));
            expected[state::key::chain_state()] = ycs::encode(cs);
            CHECK(test::dump(block) == expected);
            CHECK_NOTHROW(state::check_invariants(block));
        }
    }
}
