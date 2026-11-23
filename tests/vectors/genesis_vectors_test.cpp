// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "vector_util.hpp"
#include "yap/chain/genesis.hpp"
#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

using namespace yap;

TEST_CASE("genesis vectors: hash, YCS and initial state root", "[vectors][genesis]") {
    const auto vec = test::load_vectors("genesis.json");
    for (const auto& v : vec.at("valid")) {
        INFO(v.at("name").get<std::string>());
        auto g = chain::parse_genesis_json(v.at("json").get<std::string>());
        if (!g) FAIL(g.error());
        CHECK(to_hex(ycs::encode(*g)) == v.at("ycs").get<std::string>());
        CHECK(to_hex(chain::genesis_hash(*g)) == v.at("genesis_hash").get<std::string>());

        state::MemoryKvStore store;
        state::Overlay o(&store);
        chain::write_initial_state(*g, o);
        CHECK_NOTHROW(state::check_invariants(o));
        CHECK(to_hex(state::state_root(o)) == v.at("initial_state_root").get<std::string>());
    }
}

TEST_CASE("genesis vectors: every invalid genesis is rejected", "[vectors][genesis]") {
    const auto vec = test::load_vectors("genesis.json");
    std::size_t n = 0;
    for (const auto& v : vec.at("invalid")) {
        INFO(v.at("name").get<std::string>());
        auto g = chain::parse_genesis_json(v.at("json").get<std::string>());
        CHECK_FALSE(g.has_value());
        if (!g) UNSCOPED_INFO(g.error());
        ++n;
    }
    CHECK(n >= 40);
}

TEST_CASE("genesis JSON writer is the inverse of the strict parser", "[vectors][genesis]") {
    const auto vec = test::load_vectors("genesis.json");
    for (const auto& v : vec.at("valid")) {
        INFO(v.at("name").get<std::string>());
        const auto g = chain::parse_genesis_json(v.at("json").get<std::string>()).value();
        const auto again = chain::parse_genesis_json(chain::to_genesis_json(g));
        REQUIRE(again.has_value());
        CHECK(*again == g);
        CHECK(to_hex(chain::genesis_hash(*again)) == v.at("genesis_hash").get<std::string>());
    }
}
