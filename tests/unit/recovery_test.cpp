// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "temp_dir.hpp"
#include "testnet.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;
using namespace yap::test;
using storage::Table;

namespace {

std::map<Bytes, Bytes, BytesLess> dump_all(const storage::KvStore& s) {
    std::map<Bytes, Bytes, BytesLess> out;
    for (std::size_t t = 0; t < storage::kTableCount; ++t) {
        s.for_each(static_cast<Table>(t), [&](const Bytes& k, const Bytes& v) {
            Bytes key{static_cast<Byte>(t)};
            key.insert(key.end(), k.begin(), k.end());
            out.emplace(key, v);
        });
    }
    return out;
}

void raw_put(storage::KvStore& s, Table t, Bytes k, Bytes v) {
    storage::WriteBatch b;
    b.put(t, std::move(k), std::move(v));
    s.write(b);
}

chain::StoreMeta meta_of(const storage::KvStore& s) { return *chain::read_meta(s); }

/// LMDB store with `n` committed blocks of the test network.
struct Fixture {
    TestNet net = TestNet::make(20);
    std::vector<chain::ReplayBlock> blocks = net.journal(30, 8, 77);
    std::vector<Hash32> roots = reference_roots(net, blocks);
    TempDir dir;

    void build(std::size_t n) {
        storage::LmdbStore store({dir.path()});
        chain::Engine e(store);
        e.init_chain(net.genesis);
        chain::replay(e, std::span(blocks).first(n));
    }
};

}  // namespace

TEST_CASE("verify_state accepts a healthy store and reports its shape", "[recovery][lmdb]") {
    Fixture f;
    f.build(10);
    storage::LmdbStore store({f.dir.path()});
    const auto rep = chain::verify_state(store);
    CHECK(rep.height == 10);
    CHECK(rep.state_root == f.roots[9]);
    CHECK(rep.smt_leaves == rep.state_entries);
    CHECK(rep.smt_internal_nodes + 1 >= rep.smt_leaves);  // a compact binary tree with n leaves has n-1 internal nodes
}

TEST_CASE("an unsupported schema version refuses to start and leaves the store untouched", "[recovery]") {
    Fixture f;
    f.build(3);
    storage::LmdbStore store({f.dir.path()});
    auto meta = meta_of(store);
    meta.schema_version = 2;
    ycs::Writer w;
    w.u32(meta.schema_version);
    raw_put(store, Table::Meta, chain::meta_key(), std::move(w).take());
    const auto before = dump_all(store);
    CHECK_THROWS_AS(chain::Engine(store), chain::IncompatibleSchema);
    CHECK_THROWS_AS((void)chain::verify_state(store), chain::IncompatibleSchema);
    CHECK(dump_all(store) == before);  // never recreated or "repaired"
}

TEST_CASE("damaged metadata is detected at start-up", "[recovery]") {
    Fixture f;
    f.build(5);
    storage::LmdbStore store({f.dir.path()});
    const auto good = meta_of(store);

    SECTION("undecodable metadata") {
        raw_put(store, Table::Meta, chain::meta_key(), {1, 0, 0, 0, 9});
        CHECK_THROWS_AS(chain::Engine(store), chain::StoreCorruption);
    }
    SECTION("last_height ahead of the state") {
        auto m = good;
        m.last_height += 1;
        raw_put(store, Table::Meta, chain::meta_key(), ycs::encode(m));
        CHECK_THROWS_AS(chain::Engine(store), chain::StoreCorruption);
    }
    SECTION("last_state_root not matching the stored tree") {
        auto m = good;
        m.last_state_root[0] ^= 1;
        raw_put(store, Table::Meta, chain::meta_key(), ycs::encode(m));
        CHECK_THROWS_AS(chain::Engine(store), chain::StoreCorruption);
    }
    SECTION("missing block record of the last height") {
        storage::WriteBatch b;
        b.erase(Table::Results, chain::results_key(good.last_height));
        store.write(b);
        CHECK_THROWS_AS(chain::Engine(store), chain::StoreCorruption);
    }
    SECTION("metadata missing while data exists") {
        storage::WriteBatch b;
        b.erase(Table::Meta, chain::meta_key());
        store.write(b);
        const auto before = dump_all(store);
        CHECK_THROWS_AS(chain::Engine(store), chain::StoreCorruption);
        CHECK(dump_all(store) == before);
    }
}

TEST_CASE("verify_state detects state and SMT corruption", "[recovery]") {
    Fixture f;
    f.build(6);
    storage::LmdbStore store({f.dir.path()});
    CHECK_NOTHROW((void)chain::verify_state(store));

    SECTION("an account balance changed behind the engine's back") {
        Bytes victim;
        store.for_each(Table::State, [&](const Bytes& k, const Bytes&) {
            if (victim.empty() && k[0] == state::ns::kAccount) victim = k;
        });
        auto acct = ycs::decode<state::AccountState>(*store.get(Table::State, victim)).value();
        acct.balance += 1;
        raw_put(store, Table::State, victim, ycs::encode(acct));
        CHECK_THROWS_AS((void)chain::verify_state(store), chain::StoreCorruption);
    }
    SECTION("a stored SMT node altered") {
        Bytes k, v;
        store.for_each(Table::Smt, [&](const Bytes& kk, const Bytes& vv) {
            if (k.empty()) {
                k = kk;
                v = vv;
            }
        });
        v[40] ^= 1;
        raw_put(store, Table::Smt, k, v);
        CHECK_THROWS_AS((void)chain::verify_state(store), chain::StoreCorruption);
    }
    SECTION("an orphan SMT node") {
        const merkle::SmtNode orphan{true, Hash32{7}, Hash32{8}};
        const auto h = merkle::node_hash(orphan);
        raw_put(store, Table::Smt, Bytes(h.begin(), h.end()), merkle::encode_node(orphan));
        CHECK_THROWS_AS((void)chain::verify_state(store), chain::StoreCorruption);
    }
    SECTION("a state entry without its SMT leaf") {
        raw_put(store, Table::State, {state::ns::kNullifier, 1, 2, 3}, {0});
        CHECK_THROWS_AS((void)chain::verify_state(store), chain::StoreCorruption);
    }
}

TEST_CASE("replay after a restart continues without re-applying committed blocks", "[recovery][lmdb]") {
    Fixture f;
    f.build(12);
    storage::LmdbStore store({f.dir.path()});
    chain::Engine e(store);
    REQUIRE(e.info().last_height == 12);
    state::Overlay v12(&e.committed());
    const auto minted_12 = state::State(v12).chain_state().total_minted;

    std::size_t seen = 0;
    const auto executed = chain::replay(e, f.blocks, [&](const chain::ReplayBlock& b, const state::BlockResult& r) {
        CHECK(r.state_root == f.roots[b.ctx.height - 1]);
        ++seen;
    });
    CHECK(executed == 18);
    CHECK(seen == 18);
    CHECK(e.info().last_state_root == f.roots.back());
    CHECK(chain::replay(e, f.blocks) == 0);  // idempotent

    // Emission was applied exactly once per height.
    state::Overlay v30(&e.committed());
    const auto cs = state::State(v30).chain_state();
    const auto& p = f.net.genesis.params;
    CHECK(cs.total_minted == minted_12 + 18 * p.block_reward);
    CHECK_NOTHROW((void)chain::verify_state(store));
}

TEST_CASE("replay refuses diverging roots and journal gaps", "[recovery]") {
    Fixture f;
    f.build(4);
    storage::LmdbStore store({f.dir.path()});
    chain::Engine e(store);

    auto bad = f.blocks;
    bad[6].expected_root->at(0) ^= 1;
    CHECK_THROWS_AS(chain::replay(e, bad), FailStop);
    CHECK(e.info().last_height == 6);  // 5 and 6 committed; the diverging 7 was not
    CHECK(e.info().last_state_root == f.roots[5]);
    // The engine keeps the uncommitted diverging block pending; a fresh engine continues cleanly.
    chain::Engine fresh(store);
    std::vector<chain::ReplayBlock> gap{f.blocks[9]};
    CHECK_THROWS_AS(chain::replay(fresh, gap), FailStop);
    CHECK(chain::replay(fresh, f.blocks) == f.blocks.size() - 6);
    CHECK(fresh.info().last_state_root == f.roots.back());
}

TEST_CASE("replay from an external journal file matches continuous execution", "[recovery][lmdb]") {
    Fixture f;
    const std::string path = (f.dir.path() / "journal.ycs").string();
    std::filesystem::create_directories(f.dir.path());
    write_journal(path, f.blocks);
    const auto journal = read_journal(path);
    REQUIRE(journal.size() == f.blocks.size());

    TempDir db;
    storage::LmdbStore store({db.path()});
    chain::Engine e(store);
    e.init_chain(f.net.genesis);
    CHECK(chain::replay(e, journal) == journal.size());
    CHECK(e.info().last_state_root == f.roots.back());
}

TEST_CASE("verification during concurrent commits never reports false corruption", "[recovery][lmdb][audit]") {
    // Audit M1.5: multi-read checks used to read several committed versions; a writer committing
    // meanwhile made a healthy store look corrupt (263 of 2363 runs in the reviewer's experiment).
    const auto net = TestNet::make(20);
    auto blocks = net.journal(300, 6, 31);
    TempDir dir;
    storage::LmdbStore store({dir.path()});
    chain::Engine writer(store);
    writer.init_chain(net.genesis);
    std::atomic<bool> done{false};
    std::atomic<int> false_alarms{0};
    std::atomic<int> checks{0};
    std::thread reader([&] {
        while (!done.load()) {
            try {
                (void)chain::verify_state(store);
                chain::Engine probe(store);  // start-up checks of a second instance
            } catch (const std::exception&) {
                false_alarms.fetch_add(1);
            }
            checks.fetch_add(1);
        }
    });
    chain::replay(writer, blocks);
    done = true;
    reader.join();
    CHECK(false_alarms.load() == 0);
    CHECK(checks.load() > 10);
    CHECK_NOTHROW((void)chain::verify_state(store));
}

TEST_CASE("a failed replay leaves no pending block behind", "[recovery][audit]") {
    Fixture f;
    f.build(2);
    storage::LmdbStore store({f.dir.path()});
    chain::Engine e(store);
    auto bad = f.blocks;
    bad[4].expected_root->at(0) ^= 1;
    CHECK_THROWS_AS(chain::replay(e, bad), FailStop);
    CHECK_FALSE(e.has_pending());  // previously the diverging block stayed pending (and could be committed)
    CHECK_THROWS_AS(e.commit(), std::logic_error);
    CHECK(e.info().last_height == 4);
    CHECK(chain::replay(e, f.blocks) == f.blocks.size() - 4);  // the same engine continues cleanly
    CHECK(e.info().last_state_root == f.roots.back());
}
