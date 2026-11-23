// SPDX-License-Identifier: Apache-2.0
//
// Block execution against the independent Python reference (tools/refgen/exec_ref.py).
#include <catch2/catch_test_macros.hpp>

#include "vector_util.hpp"
#include "yap/chain/engine.hpp"
#include "yap/state/state.hpp"
#include "yap/state/state_root.hpp"

using namespace yap;
using test::jhex;
using test::jhexn;
using test::json;
using test::ju64;

namespace {

state::BlockContext context_of(const json& b, const std::string& chain_id) {
    state::BlockContext c;
    c.chain_id = chain_id;
    c.height = ju64(b.at("height"));
    c.time_ms = ju64(b.at("time_ms"));
    for (const auto& v : b.at("last_commit")) {
        c.last_commit.push_back({jhexn<20>(v.at("address")), ju64(v.at("power")), v.at("flag").get<std::uint8_t>()});
    }
    for (const auto& t : b.at("txs")) c.txs.push_back(jhex(t));
    return c;
}

/// Runs a scenario and returns the state root after every block.
std::vector<Hash32> run_scenario(const json& sc) {
    auto g = chain::parse_genesis_json(sc.at("genesis").get<std::string>());
    REQUIRE(g.has_value());
    chain::Engine e;
    CHECK(to_hex(e.init_chain(*g)) == sc.at("genesis_root").get<std::string>());

    std::vector<Hash32> roots;
    for (const auto& b : sc.at("blocks")) {
        const auto ctx = context_of(b, g->chain_id);
        INFO("height " << ctx.height);

        // ProcessProposal and PrepareProposal agree with the reference's per-tx outcome.
        CHECK(e.process_proposal(ctx) == b.at("accept").get<bool>());
        std::vector<Bytes> ok_txs;
        for (std::size_t i = 0; i < ctx.txs.size(); ++i) {
            if (b.at("codes")[i] == "OK") ok_txs.push_back(ctx.txs[i]);
        }
        auto prepared = ctx;
        prepared.txs = e.prepare_proposal(ctx, ctx.txs, 1'000'000);
        CHECK(prepared.txs == ok_txs);
        CHECK(e.process_proposal(prepared));

        const auto r = e.finalize_block(ctx);
        REQUIRE(r.tx_results.size() == b.at("codes").size());
        for (std::size_t i = 0; i < r.tx_results.size(); ++i) {
            INFO("tx " << i);
            CHECK(tx::to_string(r.tx_results[i].code) == b.at("codes")[i].get<std::string>());
        }
        CHECK(to_hex(r.state_root) == b.at("state_root").get<std::string>());
        CHECK(e.commit() == r.state_root);

        // Supply invariants and ChainState after every block.
        CHECK_NOTHROW(state::check_invariants(e.committed()));
        state::Overlay view(&e.committed());
        const auto cs = state::State(view).chain_state();
        const auto& want = b.at("chain_state");
        CHECK(cs.height == ju64(want.at("height")));
        CHECK(cs.last_time_ms == ju64(want.at("last_time_ms")));
        CHECK(cs.total_supply == ju64(want.at("total_supply")));
        CHECK(cs.total_minted == ju64(want.at("total_minted")));
        CHECK(cs.total_burned == ju64(want.at("total_burned")));
        CHECK(state::state_root(e.committed()) == r.state_root);
        roots.push_back(r.state_root);
    }
    return roots;
}

}  // namespace

TEST_CASE("block execution vectors match the independent reference", "[vectors][blocks]") {
    const auto vec = test::load_vectors("blocks.json");
    std::size_t blocks = 0;
    for (const auto& sc : vec.at("scenarios")) {
        INFO("scenario " << sc.at("name").get<std::string>());
        blocks += run_scenario(sc).size();
    }
    CHECK(blocks >= 50);
}

TEST_CASE("re-execution of every scenario yields identical state roots", "[vectors][blocks][determinism]") {
    const auto vec = test::load_vectors("blocks.json");
    for (const auto& sc : vec.at("scenarios")) {
        INFO("scenario " << sc.at("name").get<std::string>());
        const auto first = run_scenario(sc);
        const auto second = run_scenario(sc);
        CHECK(first == second);
    }
}
