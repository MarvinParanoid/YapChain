// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <thread>

#include "temp_dir.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;
using namespace yap::storage;

namespace {

std::map<Bytes, Bytes> dump(const KvStore& s, Table t) {
    std::map<Bytes, Bytes> out;
    s.for_each(t, [&](const Bytes& k, const Bytes& v) { out.emplace(k, v); });
    return out;
}

/// Behaviour every KvStore must share (run against MemoryStore and LmdbStore).
void contract(KvStore& s) {
    WriteBatch b;
    b.put(Table::State, {0x01, 0x00}, {0xAA});
    b.put(Table::State, {0x00}, {});                // empty value is allowed
    b.put(Table::State, {0x01}, {0xBB, 0x00, 0xCC});
    b.put(Table::State, {0xFF, 0xFF}, {1});
    b.put(Table::Smt, {0x01}, {0x99});              // same key, different table
    b.erase(Table::State, {0x42});                  // erasing a missing key is a no-op
    s.write(b);

    CHECK(s.get(Table::State, Bytes{0x01}) == Bytes{0xBB, 0x00, 0xCC});
    CHECK(s.get(Table::Smt, Bytes{0x01}) == Bytes{0x99});
    CHECK(s.get(Table::State, Bytes{0x00}) == Bytes{});
    CHECK_FALSE(s.get(Table::Meta, Bytes{0x01}).has_value());
    // Binary keys in unsigned lexicographic order: 00 < 01 < 01 00 < ff ff.
    const auto st = dump(s, Table::State);
    std::vector<Bytes> keys;
    s.for_each(Table::State, [&](const Bytes& k, const Bytes&) { keys.push_back(k); });
    CHECK(keys == std::vector<Bytes>{{0x00}, {0x01}, {0x01, 0x00}, {0xFF, 0xFF}});

    WriteBatch b2;
    b2.erase(Table::State, {0x01});
    b2.put(Table::State, {0x01, 0x00}, {0xAB});
    s.write(b2);
    CHECK_FALSE(s.get(Table::State, Bytes{0x01}).has_value());
    CHECK(s.get(Table::State, Bytes{0x01, 0x00}) == Bytes{0xAB});

    // A batch interrupted inside the transaction leaves no trace.
    const auto before = dump(s, Table::State);
    set_fault_hook([](std::string_view p) {
        if (p == "boom") throw std::runtime_error("injected");
    });
    WriteBatch b3;
    b3.put(Table::State, {0x05}, {5});
    b3.erase(Table::State, {0x00});
    b3.mark("boom");
    b3.put(Table::State, {0x06}, {6});
    CHECK_THROWS_AS(s.write(b3), std::runtime_error);
    set_fault_hook({});
    CHECK(dump(s, Table::State) == before);

    // Preconditions (compare-and-swap) are checked inside the transaction.
    WriteBatch cas_ok;
    cas_ok.expect(Table::State, {0x01, 0x00}, Bytes{0xAB});
    cas_ok.expect(Table::Meta, {0x09}, std::nullopt);
    cas_ok.put(Table::Meta, {0x09}, {9});
    CHECK_NOTHROW(s.write(cas_ok));
    const auto after_cas = dump(s, Table::Meta);
    WriteBatch cas_bad;
    cas_bad.put(Table::State, {0x07}, {7});
    cas_bad.expect(Table::Meta, {0x09}, std::nullopt);  // no longer absent
    CHECK_THROWS_AS(s.write(cas_bad), StorageConflict);
    WriteBatch cas_bad2;
    cas_bad2.expect(Table::State, {0x01, 0x00}, Bytes{0xAC});  // wrong value
    cas_bad2.put(Table::State, {0x07}, {7});
    CHECK_THROWS_AS(s.write(cas_bad2), StorageConflict);
    CHECK_FALSE(s.get(Table::State, Bytes{0x07}).has_value());
    CHECK(dump(s, Table::Meta) == after_cas);
}

}  // namespace

TEST_CASE("KvStore contract: MemoryStore", "[storage]") {
    MemoryStore s;
    contract(s);
}

TEST_CASE("KvStore contract: LmdbStore", "[storage][lmdb]") {
    test::TempDir dir;
    LmdbStore s({dir.path()});
    contract(s);
}

TEST_CASE("WriteBatch rejects empty keys", "[storage]") {
    WriteBatch b;
    CHECK_THROWS_AS(b.put(Table::State, {}, {1}), std::invalid_argument);
    CHECK_THROWS_AS(b.erase(Table::State, {}), std::invalid_argument);
}

TEST_CASE("only one writer may open an LMDB database; readers may join", "[storage][lmdb]") {
    test::TempDir dir;
    LmdbStore writer({dir.path()});
    CHECK_THROWS_AS(LmdbStore({dir.path()}), StorageError);  // second writer, even in-process
    LmdbStore::Options ro{dir.path()};
    ro.read_only = true;
    ro.create = false;
    CHECK_NOTHROW(LmdbStore(ro));
}

TEST_CASE("LMDB data survives closing and reopening", "[storage][lmdb]") {
    test::TempDir dir;
    {
        LmdbStore s({dir.path()});
        WriteBatch b;
        for (Byte i = 1; i < 100; ++i) b.put(Table::State, {i}, Bytes(i, i));
        s.write(b);
    }
    LmdbStore s({dir.path()});
    CHECK(dump(s, Table::State).size() == 99);
    CHECK(s.get(Table::State, Bytes{42}) == Bytes(42, 42));
}

TEST_CASE("LMDB read-only mode refuses writes and never creates", "[storage][lmdb]") {
    test::TempDir dir;
    {
        LmdbStore s({dir.path()});
        WriteBatch b;
        b.put(Table::Meta, {1}, {2});
        s.write(b);
    }
    LmdbStore::Options ro{dir.path()};
    ro.read_only = true;
    LmdbStore s(ro);
    CHECK(s.get(Table::Meta, Bytes{1}) == Bytes{2});
    WriteBatch b;
    b.put(Table::Meta, {1}, {3});
    CHECK_THROWS_AS(s.write(b), StorageError);

    LmdbStore::Options missing{dir.path() / "nope"};
    missing.create = false;
    CHECK_THROWS_AS(LmdbStore(missing), StorageError);
}

TEST_CASE("LMDB MAP_FULL aborts the batch and keeps the previous data", "[storage][lmdb]") {
    test::TempDir dir;
    LmdbStore::Options small{dir.path()};
    small.map_size = 256 * 1024;
    {
        LmdbStore s(small);
        WriteBatch ok;
        ok.put(Table::State, {1}, Bytes(100, 7));
        s.write(ok);
        WriteBatch huge;
        for (std::uint32_t i = 0; i < 200; ++i) {
            huge.put(Table::State, {static_cast<Byte>(i >> 8), static_cast<Byte>(i), 0x10}, Bytes(4096, 1));
        }
        CHECK_THROWS_AS(s.write(huge), StorageFull);
        CHECK(dump(s, Table::State).size() == 1);  // nothing from the failed batch
        CHECK(s.get(Table::State, Bytes{1}) == Bytes(100, 7));
    }
    // Reopening with a larger map makes the same batch fit.
    LmdbStore::Options big{dir.path()};
    big.map_size = 64 * 1024 * 1024;
    LmdbStore s(big);
    WriteBatch huge;
    for (std::uint32_t i = 0; i < 200; ++i) huge.put(Table::State, {static_cast<Byte>(i >> 8), static_cast<Byte>(i), 0x10}, Bytes(4096, 1));
    CHECK_NOTHROW(s.write(huge));
    CHECK(dump(s, Table::State).size() == 201);
}

TEST_CASE("LMDB readers see consistent snapshots while one writer commits", "[storage][lmdb]") {
    // Each batch moves value between keys while keeping the total constant; a reader scanning
    // from one snapshot must always see the same total.
    test::TempDir dir;
    LmdbStore s({dir.path()});
    auto encode = [](std::uint32_t v) { return Bytes{static_cast<Byte>(v >> 24), static_cast<Byte>(v >> 16),
                                                     static_cast<Byte>(v >> 8), static_cast<Byte>(v)}; };
    auto decode = [](const Bytes& b) {
        return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
    };
    constexpr std::uint32_t kKeys = 16;
    constexpr std::uint32_t kEach = 1000;
    WriteBatch init;
    for (Byte k = 0; k < kKeys; ++k) init.put(Table::State, {static_cast<Byte>(k + 1)}, encode(kEach));
    s.write(init);

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<int> scans{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                std::uint64_t total = 0;
                s.for_each(Table::State, [&](const Bytes&, const Bytes& v) { total += decode(v); });
                if (total != kKeys * kEach) bad.fetch_add(1);
                scans.fetch_add(1);
            }
        });
    }
    for (std::uint32_t i = 0; i < 300; ++i) {
        const Byte from = static_cast<Byte>(1 + i % kKeys);
        const Byte to = static_cast<Byte>(1 + (i * 7 + 3) % kKeys);
        if (from == to) continue;
        const auto fv = decode(*s.get(Table::State, Bytes{from}));
        const auto tv = decode(*s.get(Table::State, Bytes{to}));
        const std::uint32_t amount = fv / 2;
        WriteBatch b;
        b.put(Table::State, {from}, encode(fv - amount));
        b.put(Table::State, {to}, encode(tv + amount));
        s.write(b);
    }
    stop = true;
    for (auto& t : readers) t.join();
    CHECK(bad.load() == 0);
    CHECK(scans.load() > 0);
}

TEST_CASE("fault points are reported in order", "[storage]") {
    for (int impl = 0; impl < 2; ++impl) {
        test::TempDir dir;
        std::unique_ptr<KvStore> s;
        if (impl == 0) {
            s = std::make_unique<MemoryStore>();
        } else {
            s = std::make_unique<LmdbStore>(LmdbStore::Options{dir.path()});
        }
        std::vector<std::string> seen;
        set_fault_hook([&](std::string_view p) { seen.emplace_back(p); });
        WriteBatch b;
        b.put(Table::State, {1}, {1});
        b.mark("mid");
        b.put(Table::State, {2}, {2});
        s->write(b);
        set_fault_hook({});
        CHECK(seen == std::vector<std::string>{"before_txn", "mid", "before_commit", "after_commit"});
    }
}
