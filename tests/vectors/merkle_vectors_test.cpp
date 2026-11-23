// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "vector_util.hpp"
#include "yap/merkle/merkle.hpp"
#include "yap/state/state_root.hpp"

using namespace yap;
using test::jhex;
using test::jhexn;

TEST_CASE("sparse Merkle tree vectors (protocol §8.1)", "[vectors][merkle]") {
    const auto vec = test::load_vectors("merkle.json");
    for (const auto& c : vec.at("smt")) {
        INFO(c.at("name").get<std::string>());
        std::vector<merkle::SmtLeaf> leaves;
        for (const auto& l : c.at("leaves")) leaves.push_back({jhexn<32>(l.at("key")), jhexn<32>(l.at("value_hash"))});
        CHECK(to_hex(merkle::smt_root(leaves)) == c.at("root").get<std::string>());
        std::reverse(leaves.begin(), leaves.end());  // order independence
        CHECK(to_hex(merkle::smt_root(leaves)) == c.at("root").get<std::string>());
    }
}

TEST_CASE("state root vectors", "[vectors][merkle]") {
    const auto vec = test::load_vectors("merkle.json").at("state");
    state::MemoryKvStore store;
    for (const auto& e : vec.at("entries")) store.put(jhex(e.at("raw_key")), jhex(e.at("value")));
    CHECK(to_hex(state::state_root(store)) == vec.at("root").get<std::string>());
}

TEST_CASE("RFC 6962 list root vectors (protocol §8.2)", "[vectors][merkle]") {
    const auto vec = test::load_vectors("merkle.json");  // keep the document alive for the loop
    for (const auto& c : vec.at("rfc6962")) {
        const auto n = c.at("n").get<std::size_t>();
        std::vector<Bytes> items;
        for (std::size_t i = 0; i < n; ++i) items.emplace_back(i + 1, static_cast<Byte>(i));
        INFO("n = " << n);
        CHECK(to_hex(merkle::rfc6962_root(items)) == c.at("root").get<std::string>());
    }
}
