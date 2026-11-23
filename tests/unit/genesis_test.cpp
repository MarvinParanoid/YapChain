// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "ledger_fixture.hpp"
#include "yap/chain/genesis.hpp"
#include "yap/state/state_root.hpp"

using namespace yap;
using namespace yap::test;

namespace {
chain::Genesis small_genesis() {
    chain::Genesis g;
    g.chain_id = kTestChain;
    g.genesis_time_ms = 1000;
    g.params = tx::testnet_params();
    g.accounts = {{id_of(key_of(1)), 500}, {id_of(key_of(2)), 700}};
    std::sort(g.accounts.begin(), g.accounts.end(),
              [](const auto& a, const auto& b) { return a.account_id < b.account_id; });
    g.validators = {{key_of(9).public_key(), 10, id_of(key_of(1)), "v1"}};
    return g;
}
}  // namespace

TEST_CASE("genesis validation accepts a minimal network and computes supply", "[genesis]") {
    const auto g = small_genesis();
    CHECK(chain::validate_genesis(g).empty());
    state::MemoryKvStore store;
    state::Overlay o(&store);
    chain::write_initial_state(g, o);
    const auto cs = state::State(o).chain_state();
    CHECK(cs == state::ChainState{0, 1000, 1200, 1200, 0});
    CHECK(state::State(o).validator(state::consensus_address(key_of(9).public_key())).has_value());
}

TEST_CASE("genesis hash covers every field", "[genesis]") {
    const auto g = small_genesis();
    const auto h = chain::genesis_hash(g);
    auto g2 = g;
    g2.genesis_time_ms += 1;
    CHECK(chain::genesis_hash(g2) != h);
    g2 = g;
    g2.validators[0].name = "v2";
    CHECK(chain::genesis_hash(g2) != h);
    g2 = g;
    g2.params.min_fee += 1;
    CHECK(chain::genesis_hash(g2) != h);
}

TEST_CASE("genesis validation: validator keys use protocol §5.7", "[genesis]") {
    auto g = small_genesis();
    g.validators[0].consensus_pubkey = crypto::PublicKey{1};  // identity point: small order
    CHECK_FALSE(chain::validate_genesis(g).empty());
}

TEST_CASE("genesis HRP is tyap until a mainnet chain_id exists", "[genesis]") {
    CHECK(chain::hrp_for_chain("yapchain-testnet-1") == "tyap");
    CHECK(chain::hrp_for_chain("anything") == "tyap");
}
