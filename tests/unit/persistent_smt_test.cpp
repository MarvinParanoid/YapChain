// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <random>

#include "yap/merkle/persistent_smt.hpp"

using namespace yap;
using namespace yap::merkle;

namespace {

/// Node store backed by a map; changes are applied explicitly, like a committed batch.
class MapNodes final : public NodeReader {
public:
    std::map<Hash32, SmtNode> nodes;
    [[nodiscard]] std::optional<SmtNode> node(const Hash32& h) const override {
        auto it = nodes.find(h);
        if (it == nodes.end()) return std::nullopt;
        return it->second;
    }
    void apply(const SmtUpdater& u) {
        for (const auto& [h, n] : u.changes()) {
            if (n) {
                nodes[h] = *n;
            } else {
                nodes.erase(h);
            }
        }
    }
};

Hash32 rnd(std::mt19937_64& rng) {
    Hash32 h{};
    for (auto& b : h) b = static_cast<Byte>(rng());
    return h;
}

Hash32 full_root(const std::map<Hash32, Hash32>& model) {
    std::vector<SmtLeaf> leaves;
    for (const auto& [k, v] : model) leaves.push_back({k, v});
    return smt_root(leaves);
}

void check_tree(const MapNodes& store, const Hash32& root, const std::map<Hash32, Hash32>& model) {
    const auto report = verify_tree(store, root);
    REQUIRE(report.leaves.size() == model.size());
    std::size_t i = 0;
    for (const auto& [k, v] : model) {
        CHECK(report.leaves[i].key == k);
        CHECK(report.leaves[i].value_hash == v);
        ++i;
    }
    // No garbage: every stored node is reachable.
    CHECK(store.nodes.size() == report.leaves.size() + report.internal_nodes);
}

}  // namespace

TEST_CASE("persistent SMT equals the full recomputation under random updates", "[merkle][smt]") {
    std::mt19937_64 rng(2026);
    MapNodes store;
    std::map<Hash32, Hash32> model;
    Hash32 root{};
    std::vector<Hash32> keys;
    for (int batch = 0; batch < 120; ++batch) {
        SmtUpdater u(store, root);
        const auto ops = 1 + rng() % 25;
        for (std::uint64_t i = 0; i < ops; ++i) {
            const auto kind = rng() % 10;
            if (kind < 5 || keys.empty()) {  // insert a new key
                keys.push_back(rnd(rng));
                const auto v = rnd(rng);
                u.update(keys.back(), v);
                model[keys.back()] = v;
            } else if (kind < 8) {  // update an existing key
                const auto& k = keys[rng() % keys.size()];
                const auto v = rnd(rng);
                u.update(k, v);
                model[k] = v;
            } else {  // delete (sometimes a key that is already gone)
                const auto& k = keys[rng() % keys.size()];
                u.update(k, std::nullopt);
                model.erase(k);
            }
        }
        root = u.root();
        store.apply(u);
        REQUIRE(root == full_root(model));
        check_tree(store, root, model);
    }
    // Delete everything: the tree and the node store become empty.
    SmtUpdater u(store, root);
    for (const auto& [k, v] : model) u.update(k, std::nullopt);
    store.apply(u);
    CHECK(u.root() == Hash32{});
    CHECK(store.nodes.empty());
}

TEST_CASE("persistent SMT handles keys sharing long prefixes", "[merkle][smt]") {
    MapNodes store;
    std::map<Hash32, Hash32> model;
    Hash32 root{};
    // Keys that differ only in the last bit force splits down to depth 255 and collapses back up.
    Hash32 a{}, b{};
    b[31] = 1;
    Hash32 c{};
    c[0] = 0x80;
    for (const auto& k : {a, b, c}) {
        SmtUpdater u(store, root);
        Hash32 v{};
        v[0] = k[31] + 1;
        u.update(k, v);
        model[k] = v;
        root = u.root();
        store.apply(u);
        CHECK(root == full_root(model));
        check_tree(store, root, model);
    }
    SmtUpdater u(store, root);
    u.update(b, std::nullopt);  // the remaining leaf under the long chain collapses to the top
    model.erase(b);
    store.apply(u);
    CHECK(u.root() == full_root(model));
    check_tree(store, u.root(), model);
}

TEST_CASE("updating to the same value or deleting a missing key changes nothing", "[merkle][smt]") {
    std::mt19937_64 rng(5);
    MapNodes store;
    SmtUpdater u(store, Hash32{});
    const auto k = rnd(rng), v = rnd(rng);
    u.update(k, v);
    store.apply(u);
    SmtUpdater again(store, u.root());
    again.update(k, v);
    again.update(rnd(rng), std::nullopt);
    CHECK(again.root() == u.root());
    CHECK(again.changes().empty());
}

TEST_CASE("verify_tree detects corruption", "[merkle][smt]") {
    std::mt19937_64 rng(9);
    MapNodes store;
    SmtUpdater u(store, Hash32{});
    for (int i = 0; i < 20; ++i) u.update(rnd(rng), rnd(rng));
    store.apply(u);
    const auto root = u.root();
    CHECK_NOTHROW((void)verify_tree(store, root));

    auto tampered = store;
    tampered.nodes.begin()->second.b[0] ^= 1;  // stored content no longer matches its hash
    CHECK_THROWS_AS((void)verify_tree(tampered, root), SmtCorruption);

    auto missing = store;
    missing.nodes.erase(missing.nodes.begin());
    CHECK_THROWS_AS((void)verify_tree(missing, root), SmtCorruption);

    SmtUpdater broken(missing, root);  // updates through a missing node are corruption, not silence
    bool threw = false;
    for (int i = 0; i < 50 && !threw; ++i) {
        try {
            broken.update(rnd(rng), rnd(rng));
        } catch (const SmtCorruption&) {
            threw = true;
        }
    }
    CHECK(threw);
}

TEST_CASE("node encoding round trip and strict decoding", "[merkle][smt]") {
    SmtNode n{false, Hash32{1}, Hash32{2}};
    CHECK(decode_node(encode_node(n)) == n);
    Bytes bad = encode_node(n);
    bad[0] = 2;
    CHECK_FALSE(decode_node(bad).has_value());
    bad = encode_node(n);
    bad.pop_back();
    CHECK_FALSE(decode_node(bad).has_value());
}
