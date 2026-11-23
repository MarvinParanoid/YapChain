// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger_fixture.hpp"
#include "yap/common/checked.hpp"

using namespace yap;
using namespace yap::test;
using tx::TxCode;

namespace {
const crypto::AccountId kTreasury = crypto::system_account_id(crypto::kTreasuryAccountName);
}

TEST_CASE("transfer moves funds, settles the fee and bumps the nonce", "[ledger]") {
    const auto a = key_of(1), b = key_of(2);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 100'000}});
    state::Overlay block(&store);

    const auto r = state::execute_tx(block, transfer_tx(a, id_of(b), 40'000, 0), 1, kTestChain);
    REQUIRE(r.code == TxCode::Ok);
    state::State st(block);
    CHECK(st.account(id_of(a)) == state::AccountState{59'000, 1});
    CHECK(st.account(id_of(b)) == state::AccountState{40'000, 0});
    CHECK(st.account(kTreasury) == state::AccountState{500, 0});  // 50 % of 1000
    const auto cs = st.chain_state();
    CHECK(cs.total_burned == 500);
    CHECK(cs.total_supply == 99'500);
    CHECK(cs.total_minted == 100'000);
    CHECK_NOTHROW(state::check_invariants(block));
    CHECK(store.size() == 3);  // nothing reached the committed store
}

TEST_CASE("burn reduces supply by amount and burned fee part", "[ledger]") {
    const auto a = key_of(3);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 10'000}});
    state::Overlay block(&store);
    REQUIRE(state::execute_tx(block, burn_tx(a, 2'000, 0), 1, kTestChain).code == TxCode::Ok);
    const auto cs = state::State(block).chain_state();
    CHECK(cs.total_burned == 2'500);
    CHECK(cs.total_supply == 7'500);
    CHECK_NOTHROW(state::check_invariants(block));
}

TEST_CASE("sequential nonces within one block and replay protection", "[ledger]") {
    const auto a = key_of(4), b = key_of(5);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 1'000'000}});
    state::Overlay block(&store);
    for (std::uint64_t n = 0; n < 3; ++n) {
        CHECK(state::execute_tx(block, transfer_tx(a, id_of(b), 10, n), 1, kTestChain).code == TxCode::Ok);
    }
    CHECK(state::execute_tx(block, transfer_tx(a, id_of(b), 10, 0), 1, kTestChain).code == TxCode::ErrNonce);
    CHECK(state::execute_tx(block, transfer_tx(a, id_of(b), 10, 4), 1, kTestChain).code == TxCode::ErrNonce);
    CHECK(state::State(block).account(id_of(a))->nonce == 3);
}

TEST_CASE("a failed transaction leaves the state byte-identical", "[ledger][atomicity]") {
    const auto a = key_of(6), b = key_of(7);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 5'000}});
    state::Overlay block(&store);
    REQUIRE(state::execute_tx(block, transfer_tx(a, id_of(b), 1'000, 0), 1, kTestChain).code == TxCode::Ok);
    const auto before = dump(block);
    for (const auto& bad : {transfer_tx(a, id_of(b), 3'001, 1),           // one yip short
                            transfer_tx(a, id_of(b), 10, 1, 999),         // fee below minimum
                            transfer_tx(a, id_of(b), 10, 7),              // nonce gap
                            burn_tx(a, kU64Max, 1),                       // amount + fee overflow
                            Bytes{1, 2, 3}}) {
        CHECK(state::execute_tx(block, bad, 1, kTestChain).code != TxCode::Ok);
        CHECK(dump(block) == before);
    }
}

TEST_CASE("overflow during effects rolls back the debit already made", "[ledger][atomicity]") {
    // Deliberately inconsistent state (invariants are not checked here): the recipient is
    // one step away from u64::MAX, so crediting fails after the sender was debited in the tx overlay.
    const auto a = key_of(8), b = key_of(9);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 10'000}});
    {
        state::Overlay o(&store);
        state::State(o).put_account(id_of(b), {kU64Max - 5, 0});
        o.apply_to(store);
    }
    state::Overlay block(&store);
    const auto before = dump(block);
    const auto r = state::execute_tx(block, transfer_tx(a, id_of(b), 6, 0), 1, kTestChain);
    CHECK(r.code == TxCode::ErrOverflow);
    CHECK(dump(block) == before);
}

TEST_CASE("an emptied account keeps its nonce (never deleted)", "[ledger]") {
    const auto a = key_of(10), b = key_of(11);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 2'500}});
    state::Overlay block(&store);
    REQUIRE(state::execute_tx(block, transfer_tx(a, id_of(b), 1'500, 0), 1, kTestChain).code == TxCode::Ok);
    CHECK(state::State(block).account(id_of(a)) == state::AccountState{0, 1});
    // The old transaction cannot be replayed even after the account is refunded.
    REQUIRE(state::execute_tx(block, transfer_tx(b, id_of(a), 1, 0), 1, kTestChain).code == TxCode::Ok);
    CHECK(state::State(block).account(id_of(a)) == state::AccountState{1, 1});
    CHECK(state::execute_tx(block, transfer_tx(a, id_of(b), 1'500, 0), 1, kTestChain).code == TxCode::ErrNonce);
}

TEST_CASE("zero credits never create accounts (ADR-0021)", "[ledger]") {
    const auto a = key_of(12), b = key_of(13);
    auto params = tx::testnet_params();
    params.fee_burn_bps = 10'000;  // the whole fee is burned: Treasury share is 0
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 5'000}}, params);
    state::Overlay block(&store);
    REQUIRE(state::execute_tx(block, transfer_tx(a, id_of(b), 1, 0), 1, kTestChain).code == TxCode::Ok);
    state::State st(block);
    CHECK_FALSE(st.account(kTreasury).has_value());
    CHECK(st.chain_state().total_burned == 1'000);
    CHECK_NOTHROW(state::check_invariants(block));
    CHECK(st.credit(id_of(key_of(14)), 0));
    CHECK_FALSE(st.account(id_of(key_of(14))).has_value());
}

TEST_CASE("execution is deterministic", "[ledger][determinism]") {
    auto run = [] {
        const auto a = key_of(15), b = key_of(16), c = key_of(17);
        state::MemoryKvStore store;
        seed_state(store, {{id_of(a), 1'000'000}, {id_of(b), 3'000}});
        state::Overlay block(&store);
        std::vector<tx::TxCode> codes;
        for (std::uint64_t i = 0; i < 20; ++i) {
            codes.push_back(state::execute_tx(block, transfer_tx(a, id_of(c), 100 + i, i), 1, kTestChain).code);
            codes.push_back(state::execute_tx(block, burn_tx(b, 500, i), 1, kTestChain).code);
        }
        block.apply_to(store);
        return std::make_pair(codes, dump(store));
    };
    const auto first = run();
    CHECK(first == run());
    // a: 20 transfers succeed; b: 3000 yip pay for exactly two burns of 500 + 1000 fee.
    CHECK(std::count(first.first.begin(), first.first.end(), TxCode::Ok) == 22);
}

TEST_CASE("invariant checker detects inconsistencies", "[ledger][invariants]") {
    const auto a = key_of(18);
    state::MemoryKvStore store;
    seed_state(store, {{id_of(a), 1'000}});
    CHECK_NOTHROW(state::check_invariants(store));

    state::Overlay o(&store);
    state::State st(o);
    st.put_account(id_of(a), {1'001, 0});
    CHECK_THROWS_AS(state::check_invariants(o), FailStop);

    state::Overlay o2(&store);
    auto cs = state::State(o2).chain_state();
    cs.total_burned = 1;
    state::State(o2).put_chain_state(cs);
    CHECK_THROWS_AS(state::check_invariants(o2), FailStop);

    state::Overlay o3(&store);
    o3.put(state::key::account(id_of(a)), Bytes{1, 2, 3});  // corrupted value
    CHECK_THROWS_AS(state::check_invariants(o3), FailStop);
    CHECK_THROWS_AS(state::State(o3).account(id_of(a)), FailStop);
}

TEST_CASE("overlay semantics", "[ledger][kv]") {
    state::MemoryKvStore store;
    store.put({1}, {10});
    store.put({2}, {20});
    state::Overlay o(&store);
    o.put({3}, {30});
    o.erase({1});
    CHECK_FALSE(o.get(Bytes{1}).has_value());
    CHECK(o.get(Bytes{2}) == Bytes{20});
    CHECK(dump(o) == std::map<Bytes, Bytes>{{{2}, {20}}, {{3}, {30}}});
    state::Overlay nested(&o);
    nested.put({2}, {21});
    state::Overlay unrelated(&store);
    CHECK_THROWS_AS(nested.merge_into(unrelated), std::logic_error);
    nested.merge_into(o);
    o.apply_to(store);
    CHECK(dump(store) == std::map<Bytes, Bytes>{{{2}, {21}}, {{3}, {30}}});
}
