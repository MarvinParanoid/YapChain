// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger_fixture.hpp"
#include "temp_dir.hpp"
#include "yap/chain/engine.hpp"
#include "yap/storage/lmdb_store.hpp"
#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

using namespace yap;
using namespace yap::test;
using tx::TxCode;

namespace {
const auto kAlice = key_of(31);
const auto kBob = key_of(32);

chain::Genesis genesis() {
    chain::Genesis g;
    g.chain_id = kTestChain;
    g.genesis_time_ms = 1'000;
    g.params = tx::testnet_params();
    g.accounts = {{id_of(kAlice), 1'000'000}, {id_of(kBob), 5'000}};
    std::sort(g.accounts.begin(), g.accounts.end(),
              [](const auto& a, const auto& b) { return a.account_id < b.account_id; });
    for (Byte i = 0; i < 4; ++i) {
        g.validators.push_back({key_of(static_cast<Byte>(40 + i)).public_key(), 10, id_of(key_of(50)), "val"});
    }
    std::sort(g.validators.begin(), g.validators.end(), [](const auto& a, const auto& b) {
        return state::consensus_address(a.consensus_pubkey) < state::consensus_address(b.consensus_pubkey);
    });
    return g;
}

state::BlockContext ctx(const chain::Genesis& g, std::uint64_t height, std::vector<Bytes> txs) {
    state::BlockContext c;
    c.chain_id = kTestChain;
    c.height = height;
    c.time_ms = 1'000 + height * 2'000;
    if (height > 1) {
        for (const auto& v : g.validators) c.last_commit.push_back({state::consensus_address(v.consensus_pubkey), 10, 2});
    }
    c.txs = std::move(txs);
    return c;
}
}  // namespace

TEST_CASE("engine lifecycle: init, finalize, commit, info", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    const Hash32 genesis_root = e.init_chain(g);
    CHECK(e.info().last_height == 0);
    CHECK(e.info().last_state_root == genesis_root);
    CHECK_THROWS_AS(e.init_chain(g), std::logic_error);

    const auto r = e.finalize_block(ctx(g, 1, {transfer_tx(kAlice, id_of(kBob), 10, 0)}));
    REQUIRE(r.tx_results.size() == 1);
    CHECK(r.tx_results[0].code == TxCode::Ok);
    CHECK(e.info().last_height == 0);  // nothing committed yet
    CHECK_THROWS_AS(e.finalize_block(ctx(g, 1, {})), std::logic_error);
    CHECK(e.commit() == r.state_root);
    CHECK(e.info().last_height == 1);
    CHECK(state::state_root(e.committed()) == r.state_root);
    CHECK_NOTHROW(state::check_invariants(e.committed()));
    CHECK_THROWS_AS(e.commit(), std::logic_error);
}

TEST_CASE("process_proposal rejects a block with any invalid tx and never mutates state", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    const auto good = transfer_tx(kAlice, id_of(kBob), 10, 0);
    const auto bad = transfer_tx(kBob, id_of(kAlice), 1'000'000, 0);  // insufficient funds
    const auto before = dump(e.committed());

    CHECK(e.process_proposal(ctx(g, 1, {good})));
    CHECK_FALSE(e.process_proposal(ctx(g, 1, {good, bad})));
    CHECK(dump(e.committed()) == before);

    // FinalizeBlock of the rejected proposal still executes deterministically, skipping the bad tx.
    const auto r = e.finalize_block(ctx(g, 1, {good, bad}));
    CHECK(r.tx_results[0].code == TxCode::Ok);
    CHECK(r.tx_results[1].code == TxCode::ErrInsufficientFunds);
}

TEST_CASE("process_proposal ACCEPT implies all-OK finalize_block", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    std::vector<Bytes> txs;
    for (std::uint64_t n = 0; n < 10; ++n) txs.push_back(transfer_tx(kAlice, id_of(kBob), 1 + n, n));
    const auto c = ctx(g, 1, txs);
    REQUIRE(e.process_proposal(c));
    for (const auto& r : e.finalize_block(c).tx_results) CHECK(r.code == TxCode::Ok);
}

TEST_CASE("prepare_proposal keeps valid txs in order within the byte budget", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    const auto t0 = transfer_tx(kAlice, id_of(kBob), 1, 0);
    const auto dup = t0;                                       // replay: nonce already used
    const auto t1 = transfer_tx(kAlice, id_of(kBob), 1, 1);
    const auto gap = transfer_tx(kAlice, id_of(kBob), 1, 5);   // nonce gap
    const auto t2 = transfer_tx(kAlice, id_of(kBob), 1, 2);
    const auto c = ctx(g, 1, {});
    CHECK(e.prepare_proposal(c, {t0, dup, t1, gap, t2}, 1'000'000) == std::vector<Bytes>{t0, t1, t2});
    CHECK(e.prepare_proposal(c, {t0, t1, t2}, t0.size() + t1.size()) == std::vector<Bytes>{t0, t1});
    CHECK(e.prepare_proposal(c, {t0}, t0.size() - 1).empty());
    auto prepared = c;
    prepared.txs = e.prepare_proposal(c, {t0, dup, t1, gap, t2}, 1'000'000);
    CHECK(e.process_proposal(prepared));
}

TEST_CASE("check_tx tracks the mempool state and resets on commit", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    CHECK(e.check_tx(transfer_tx(kAlice, id_of(kBob), 1, 0)).code == TxCode::Ok);
    CHECK(e.check_tx(transfer_tx(kAlice, id_of(kBob), 1, 1)).code == TxCode::Ok);   // follows the accepted one
    CHECK(e.check_tx(transfer_tx(kAlice, id_of(kBob), 1, 0)).code == TxCode::ErrNonce);
    CHECK(e.check_tx(Bytes{1}).code == TxCode::ErrDecode);
    // A block that includes only the first tx; after commit the check-state restarts from it.
    e.finalize_block(ctx(g, 1, {transfer_tx(kAlice, id_of(kBob), 1, 0)}));
    e.commit();
    CHECK(e.check_tx(transfer_tx(kAlice, id_of(kBob), 1, 1)).code == TxCode::Ok);
}

TEST_CASE("invalid block context is fail-stop and leaves nothing pending", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    auto wrong_height = ctx(g, 2, {});
    CHECK_THROWS_AS(e.finalize_block(wrong_height), FailStop);
    auto wrong_chain = ctx(g, 1, {});
    wrong_chain.chain_id = "other-chain";
    CHECK_THROWS_AS(e.finalize_block(wrong_chain), FailStop);
    auto backwards = ctx(g, 1, {});
    backwards.time_ms = 999;  // genesis time is 1000
    CHECK_THROWS_AS(e.finalize_block(backwards), FailStop);
    auto commit_at_1 = ctx(g, 1, {});
    commit_at_1.last_commit.push_back({state::consensus_address(g.validators[0].consensus_pubkey), 10, 2});
    CHECK_THROWS_AS(e.process_proposal(commit_at_1), FailStop);
    // After the failures a correct block still works: no partial pending state was kept.
    CHECK_NOTHROW(e.finalize_block(ctx(g, 1, {})));
    CHECK_NOTHROW(e.commit());
}

TEST_CASE("init_chain rejects an invalid genesis", "[engine]") {
    auto g = genesis();
    g.validators.clear();
    storage::MemoryStore store;
    chain::Engine e(store);
    CHECK_THROWS_AS(e.init_chain(g), std::invalid_argument);
}

// ------------------------------------------------------------------ durability (M1.3b)

namespace {
state::ChainState chain_state_of(const chain::Engine& e) {
    state::Overlay view(&e.committed());
    return state::State(view).chain_state();
}
}  // namespace

TEST_CASE("committed heights survive reopening the LMDB store", "[engine][lmdb]") {
    const auto g = genesis();
    test::TempDir dir;
    Hash32 root3{};
    {
        storage::LmdbStore store({dir.path()});
        chain::Engine e(store);
        e.init_chain(g);
        for (std::uint64_t h = 1; h <= 3; ++h) {
            e.finalize_block(ctx(g, h, {transfer_tx(kAlice, id_of(kBob), h, h - 1)}));
            root3 = e.commit();
        }
        // A finalized but uncommitted block is not durable.
        e.finalize_block(ctx(g, 4, {transfer_tx(kAlice, id_of(kBob), 9, 3)}));
    }
    storage::LmdbStore store({dir.path()});
    chain::Engine e(store);
    REQUIRE(e.initialized());
    CHECK(e.info().last_height == 3);
    CHECK(e.info().last_state_root == root3);
    CHECK(state::state_root(e.committed()) == root3);
    CHECK_NOTHROW(state::check_invariants(e.committed()));
    const auto rec = e.block_record(3);
    REQUIRE(rec.has_value());
    CHECK(rec->state_root == root3);
    CHECK(rec->codes == std::vector<std::uint32_t>{0});
    CHECK_FALSE(e.block_record(4).has_value());
}

TEST_CASE("a committed height cannot be applied again (no double emission)", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(g);
    e.finalize_block(ctx(g, 1, {}));
    e.commit();
    const auto minted = chain_state_of(e).total_minted;
    CHECK_THROWS_AS(e.commit(), std::logic_error);           // second Commit without FinalizeBlock
    CHECK_THROWS_AS(e.finalize_block(ctx(g, 1, {})), FailStop);  // replaying height 1
    chain::Engine restarted(store);
    CHECK_THROWS_AS(restarted.finalize_block(ctx(g, 1, {})), FailStop);
    CHECK(chain_state_of(restarted).total_minted == minted);
}

TEST_CASE("a failing commit keeps the previous height and the pending block", "[engine]") {
    const auto g = genesis();
    storage::MemoryStore store;
    chain::Engine e(store);
    const auto genesis_root = e.init_chain(g);
    const auto r = e.finalize_block(ctx(g, 1, {transfer_tx(kAlice, id_of(kBob), 5, 0)}));
    const auto before = dump(e.committed());

    storage::set_fault_hook([](std::string_view p) {
        if (p == "after_chain_state") throw std::runtime_error("disk on fire");
    });
    CHECK_THROWS_AS(e.commit(), std::runtime_error);
    storage::set_fault_hook({});
    CHECK(e.info().last_height == 0);
    CHECK(e.info().last_state_root == genesis_root);
    CHECK(dump(e.committed()) == before);

    CHECK(e.commit() == r.state_root);  // retry of the same pending block succeeds once
    CHECK(e.info().last_height == 1);
    CHECK_THROWS_AS(e.commit(), std::logic_error);
}

TEST_CASE("MAP_FULL during commit is reported and recoverable", "[engine][lmdb]") {
    const auto g = genesis();
    test::TempDir dir;
    std::uint64_t committed_height = 0;
    Hash32 committed_root{};
    state::BlockContext failed_block;
    {
        storage::LmdbStore::Options small{dir.path()};
        small.map_size = 256 * 1024;
        storage::LmdbStore store(small);
        chain::Engine e(store);
        e.init_chain(g);
        bool full = false;
        for (std::uint64_t h = 1; h <= 200 && !full; ++h) {
            std::vector<Bytes> txs;
            for (std::uint64_t i = 0; i < 20; ++i) {
                const auto nonce = (h - 1) * 20 + i;
                txs.push_back(transfer_tx(kAlice, crypto::system_account_id("sink-" + std::to_string(nonce)), 1, nonce));
            }
            auto c = ctx(g, h, txs);
            e.finalize_block(c);
            try {
                e.commit();
            } catch (const storage::StorageFull&) {
                full = true;
                failed_block = c;
                CHECK(e.info().last_height == h - 1);
                CHECK(state::state_root(e.committed()) == e.info().last_state_root);
            }
        }
        REQUIRE(full);
        committed_height = e.info().last_height;
        committed_root = e.info().last_state_root;
    }
    storage::LmdbStore::Options big{dir.path()};
    big.map_size = 64 * 1024 * 1024;
    storage::LmdbStore store(big);
    chain::Engine e(store);
    CHECK(e.info().last_height == committed_height);
    CHECK(e.info().last_state_root == committed_root);
    e.finalize_block(failed_block);
    CHECK_NOTHROW(e.commit());
    CHECK(e.info().last_height == committed_height + 1);
    CHECK_NOTHROW(state::check_invariants(e.committed()));
}
