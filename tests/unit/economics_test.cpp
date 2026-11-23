// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger_fixture.hpp"
#include "yap/common/checked.hpp"
#include "yap/state/economics.hpp"

using namespace yap;
using namespace yap::test;
using state::CommitVote;

namespace {
struct Net {
    state::MemoryKvStore store;
    std::vector<state::ConsensusAddress> addrs;
    explicit Net(tx::ChainParams p = tx::testnet_params()) {
        state::Overlay o(&store);
        state::State st(o);
        st.put_params(p);
        st.put_chain_state({0, 0, 0, 0, 0});
        for (Byte i = 0; i < 4; ++i) {
            const auto pub = key_of(static_cast<Byte>(100 + i)).public_key();
            addrs.push_back(state::consensus_address(pub));
            st.put_validator(addrs.back(), {"val", pub, 10, id_of(key_of(static_cast<Byte>(110 + i)))});
        }
        o.apply_to(store);
    }
};
}  // namespace

TEST_CASE("last_commit validation is fail-stop", "[economics]") {
    Net net;
    state::Overlay o(&net.store);
    state::State st(o);
    const auto a = net.addrs[0], b = net.addrs[1];
    CHECK_NOTHROW(state::validate_last_commit(st, 1, {}));
    CHECK_NOTHROW(state::validate_last_commit(st, 2, std::vector<CommitVote>{{a, 10, 2}, {b, 10, 3}}));
    CHECK_THROWS_AS(state::validate_last_commit(st, 1, std::vector<CommitVote>{{a, 10, 2}}), FailStop);
    CHECK_THROWS_AS(state::validate_last_commit(st, 2, std::vector<CommitVote>{{a, 10, 0}}), FailStop);
    CHECK_THROWS_AS(state::validate_last_commit(st, 2, std::vector<CommitVote>{{a, 10, 4}}), FailStop);
    CHECK_THROWS_AS(state::validate_last_commit(st, 2, std::vector<CommitVote>{{a, 10, 2}, {a, 10, 2}}), FailStop);
    CHECK_THROWS_AS(state::validate_last_commit(st, 2, std::vector<CommitVote>{{a, 11, 2}}), FailStop);
    state::ConsensusAddress unknown{};
    CHECK_THROWS_AS(state::validate_last_commit(st, 2, std::vector<CommitVote>{{unknown, 10, 2}}), FailStop);
}

TEST_CASE("emission stops at max_issuance and never resumes after burns", "[economics]") {
    auto p = tx::testnet_params();
    p.max_issuance = 5'000'000;  // two and a half block rewards
    Net net(p);
    std::vector<std::uint64_t> minted;
    for (int h = 0; h < 4; ++h) {
        state::Overlay o(&net.store);
        state::State st(o);
        std::vector<state::Event> ev;
        minted.push_back(state::apply_block_reward(st, {}, ev));
        if (h == 2) {
            // Burn some supply directly: the cap is on total_minted, not on total_supply.
            auto cs = st.chain_state();
            const auto treasury = crypto::system_account_id(crypto::kTreasuryAccountName);
            REQUIRE(st.debit(treasury, 1'000'000));
            cs.total_supply -= 1'000'000;
            cs.total_burned += 1'000'000;
            st.put_chain_state(cs);
        }
        CHECK_NOTHROW(state::check_invariants(o));
        o.apply_to(net.store);
    }
    CHECK(minted == std::vector<std::uint64_t>{2'000'000, 2'000'000, 1'000'000, 0});
    state::Overlay o(&net.store);
    CHECK(state::State(o).chain_state().total_minted == 5'000'000);
}

TEST_CASE("emission arithmetic failure is fail-stop", "[economics]") {
    Net net;
    state::Overlay o(&net.store);
    state::State st(o);
    auto cs = st.chain_state();
    cs.total_minted = tx::testnet_params().max_issuance + 1;  // corrupted: above the cap
    st.put_chain_state(cs);
    std::vector<state::Event> ev;
    CHECK_THROWS_AS(state::apply_block_reward(st, {}, ev), FailStop);
}

TEST_CASE("genesis supply accounting", "[economics]") {
    const std::vector<std::uint64_t> balances{100, 200, 300};
    const auto cs = state::genesis_chain_state(balances, 42, 600);
    REQUIRE(cs.has_value());
    CHECK(*cs == state::ChainState{0, 42, 600, 600, 0});
    CHECK_FALSE(state::genesis_chain_state(balances, 42, 599).has_value());
    const std::vector<std::uint64_t> huge{kU64Max, 1};
    CHECK_FALSE(state::genesis_chain_state(huge, 0, kU64Max).has_value());
    CHECK(state::genesis_chain_state({}, 0, 1) == state::ChainState{0, 0, 0, 0, 0});
}
