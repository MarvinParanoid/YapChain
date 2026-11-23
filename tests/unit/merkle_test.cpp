// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

#include "yap/merkle/merkle.hpp"

using namespace yap;

namespace {
Hash32 h(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Hash32 out{};
    for (auto& b : out) b = static_cast<Byte>(rng());
    return out;
}
}  // namespace

TEST_CASE("SMT root is independent of insertion order (property)", "[merkle]") {
    std::mt19937_64 rng(7);
    for (int round = 0; round < 50; ++round) {
        std::vector<merkle::SmtLeaf> leaves;
        const auto n = 1 + rng() % 64;
        for (std::uint64_t i = 0; i < n; ++i) leaves.push_back({h(rng()), h(rng())});
        const auto root = merkle::smt_root(leaves);
        std::shuffle(leaves.begin(), leaves.end(), rng);
        CHECK(merkle::smt_root(leaves) == root);
    }
}

TEST_CASE("SMT root changes with any key or value", "[merkle]") {
    std::vector<merkle::SmtLeaf> leaves{{h(1), h(2)}, {h(3), h(4)}, {h(5), h(6)}};
    const auto root = merkle::smt_root(leaves);
    auto v = leaves;
    v[1].value_hash[0] ^= 1;
    CHECK(merkle::smt_root(v) != root);
    auto k = leaves;
    k[2].key[31] ^= 1;
    CHECK(merkle::smt_root(k) != root);
    auto fewer = leaves;
    fewer.pop_back();
    CHECK(merkle::smt_root(fewer) != root);
    CHECK(merkle::smt_root({}) == Hash32{});
}

TEST_CASE("SMT rejects duplicate keys", "[merkle]") {
    CHECK_THROWS_AS(merkle::smt_root({{h(1), h(2)}, {h(1), h(3)}}), std::invalid_argument);
}
