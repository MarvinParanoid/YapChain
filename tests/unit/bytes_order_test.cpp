// SPDX-License-Identifier: Apache-2.0
//
// Regression tests of yap::BytesLess, the key order of every ordered byte-keyed container (state
// overlays, the in-memory store). It replaced std::less<Bytes> because GCC 14 at -O3 reports a false
// -Wstringop-overread inside the inlined vector comparison; the order itself must not change: state
// roots and iteration order depend on it (docs/protocol.md §7.5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <random>
#include <vector>

#include "yap/common/bytes.hpp"
#include "yap/common/hex.hpp"
#include "yap/state/kv.hpp"
#include "yap/state/types.hpp"

using namespace yap;

namespace {

// Reference order: unsigned lexicographic, a proper prefix sorts first (the order of LMDB and of
// std::less<std::vector<std::uint8_t>>), written out independently of both.
bool reference_less(const Bytes& a, const Bytes& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] != b[i]) return static_cast<unsigned>(a[i]) < static_cast<unsigned>(b[i]);
    }
    return a.size() < b.size();
}

// Every byte string of length 0..2 over values at the signed/unsigned boundaries.
std::vector<Bytes> small_keys() {
    const Byte alphabet[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    std::vector<Bytes> keys{Bytes{}};
    for (Byte a : alphabet) {
        keys.push_back({a});
        for (Byte b : alphabet) keys.push_back({a, b});
    }
    return keys;
}

}  // namespace

TEST_CASE("BytesLess is unsigned lexicographic with prefixes first", "[common][bytes]") {
    const BytesLess less;
    CHECK(less(Bytes{0x7f}, Bytes{0x80}));  // unsigned: 0x80 is not negative
    CHECK_FALSE(less(Bytes{0x80}, Bytes{0x7f}));
    CHECK(less(Bytes{0x00, 0xff}, Bytes{0xff}));
    CHECK(less(Bytes{0x01}, Bytes{0x01, 0x00}));  // a proper prefix sorts first
    CHECK_FALSE(less(Bytes{0x01, 0x00}, Bytes{0x01}));
    CHECK(less(Bytes{0x01, 0x02, 0x03}, Bytes{0x01, 0x02, 0x04}));
}

TEST_CASE("BytesLess handles empty keys", "[common][bytes]") {
    const BytesLess less;
    const Bytes empty;
    const ByteView null_view{};  // data() may be null
    CHECK_FALSE(less(empty, empty));
    CHECK_FALSE(less(null_view, null_view));
    CHECK_FALSE(less(null_view, empty));
    CHECK(less(empty, Bytes{0x00}));
    CHECK(less(null_view, Bytes{0x00}));
    CHECK_FALSE(less(Bytes{0x00}, empty));
}

TEST_CASE("BytesLess agrees with the reference order and std::less and is a strict weak order", "[common][bytes]") {
    const BytesLess less;
    const std::less<Bytes> std_less;
    const auto keys = small_keys();
    for (const auto& a : keys) {
        CHECK_FALSE(less(a, a));  // irreflexive
        for (const auto& b : keys) {
            const bool ab = less(a, b);
            CHECK(ab == reference_less(a, b));
            CHECK(ab == std_less(a, b));
            if (ab) CHECK_FALSE(less(b, a));                       // asymmetric
            if (!ab && !less(b, a)) CHECK(a == b);                 // equivalence is equality
            for (const auto& c : keys) {
                if (ab && less(b, c)) CHECK(less(a, c));           // transitive
            }
        }
    }
}

// Against the independent reference, not std::less: sorting with std::less<Bytes> is exactly the code
// in which GCC 14 at -O3 reports the false -Wstringop-overread (agreement with std::less is checked
// pair by pair above).
TEST_CASE("BytesLess sorts random keys exactly like the reference order", "[common][bytes]") {
    std::mt19937_64 rng(0x5eed);
    std::vector<Bytes> keys;
    for (int i = 0; i < 3000; ++i) {
        Bytes k(rng() % 41);
        for (auto& b : k) b = static_cast<Byte>(rng() % 4 == 0 ? 0xff : rng());  // many shared prefixes
        keys.push_back(std::move(k));
    }
    auto ours = keys;
    auto theirs = keys;
    std::sort(ours.begin(), ours.end(), BytesLess{});
    std::sort(theirs.begin(), theirs.end(), reference_less);
    CHECK(ours == theirs);
}

TEST_CASE("BytesLess lookup is transparent", "[common][bytes]") {
    std::map<Bytes, int, BytesLess> m;
    m.emplace(Bytes{}, 0);
    m.emplace(Bytes{0x80}, 1);
    m.emplace(Bytes{0x7f, 0xff}, 2);
    const Byte raw[] = {0x80};
    auto it = m.find(ByteView(raw));  // no Bytes is constructed for the lookup
    REQUIRE(it != m.end());
    CHECK(it->second == 1);
    CHECK(m.find(ByteView{})->second == 0);
    CHECK(m.find(ByteView(raw, 0)) == m.begin());
    const Byte missing[] = {0x80, 0x00};
    CHECK(m.find(ByteView(missing)) == m.end());
    std::vector<int> order;
    for (const auto& [k, v] : m) order.push_back(v);
    CHECK(order == std::vector<int>{0, 2, 1});
}

TEST_CASE("state views iterate in unsigned key order, empty key first", "[state][kv]") {
    state::MemoryKvStore store;
    store.put({0x80}, {1});
    store.put({0x7f}, {2});
    store.put({}, {3});
    store.put({0x7f, 0x00}, {4});
    CHECK(store.get(ByteView{}) == Bytes{3});
    state::Overlay overlay(&store);
    overlay.put({0x00}, {5});
    overlay.erase({0x7f});
    overlay.put({0xff}, {6});
    std::vector<Bytes> keys;
    overlay.for_each([&keys](const Bytes& k, const Bytes&) { keys.push_back(k); });
    CHECK(keys == std::vector<Bytes>{{}, {0x00}, {0x7f, 0x00}, {0x80}, {0xff}});
    std::vector<Bytes> changed;
    for (const auto& [k, v] : overlay.changes()) changed.push_back(k);
    CHECK(changed == std::vector<Bytes>{{0x00}, {0x7f}, {0xff}});
    const Byte probe[] = {0x7f};
    CHECK_FALSE(overlay.get(ByteView(probe)).has_value());  // deleted in the overlay
}

TEST_CASE("namespaced state keys keep their exact bytes", "[state][keys]") {
    crypto::AccountId id{};
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<Byte>(0xe0 + i);  // high bytes
    Bytes want{0x01};
    want.insert(want.end(), id.begin(), id.end());
    CHECK(state::key::account(id) == want);
    CHECK(to_hex(state::key::account(id)) ==
          "01e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");

    state::ConsensusAddress va{};
    for (std::size_t i = 0; i < va.size(); ++i) va[i] = static_cast<Byte>(i);
    CHECK(to_hex(state::key::validator(va)) == "03000102030405060708090a0b0c0d0e0f10111213");
    Bytes zero_key(33, 0x00);
    zero_key[0] = 0x01;
    CHECK(state::key::account(crypto::AccountId{}) == zero_key);
    CHECK(state::key::chain_state() == Bytes{0x00});
    CHECK(state::key::params() == Bytes{0x02});
}
