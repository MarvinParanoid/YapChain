// SPDX-License-Identifier: Apache-2.0
// yap query balance|account|chain (local database, read-only)
#include <CLI11.hpp>

#include <memory>

#include "commands.hpp"
#include "local_chain.hpp"
#include "yap/common/hex.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {
namespace {
crypto::AccountId parse_addr(const std::string& text) {
    auto id = crypto::decode_address(text, crypto::kTestnetHrp);
    if (!id) throw CliError(kExitRejected, "invalid address '" + text + "': " + std::string(crypto::to_string(id.error())));
    return *id;
}
}  // namespace

void register_query(CLI::App& app, Context& ctx) {
    auto* q = app.add_subcommand("query", "Read the committed local state");
    q->require_subcommand(1);

    auto addr = std::make_shared<std::string>();
    auto* bal = q->add_subcommand("balance", "Balance of an address");
    bal->add_option("address", *addr, "Address")->required();
    bal->callback([&ctx, addr] {
        const auto lc = open_local_chain(ctx, true, true);
        const auto a = lc->account(parse_addr(*addr)).value_or(state::AccountState{});
        out("address", *addr);
        out("balance_yip", a.balance);
        out("balance", wallet::format_amount(a.balance));
    });

    auto addr2 = std::make_shared<std::string>();
    auto* acc = q->add_subcommand("account", "Account state of an address");
    acc->add_option("address", *addr2, "Address")->required();
    acc->callback([&ctx, addr2] {
        const auto lc = open_local_chain(ctx, true, true);
        const auto id = parse_addr(*addr2);
        const auto a = lc->account(id);
        out("address", *addr2);
        out("account_id", to_hex(id));
        out("exists", a ? "yes" : "no");
        out("balance_yip", a ? a->balance : 0);
        out("nonce", a ? a->nonce : 0);
        out("height", lc->engine->info().last_height);
    });

    auto* ch = q->add_subcommand("chain", "Chain summary: height, root, supply");
    ch->callback([&ctx] {
        const auto lc = open_local_chain(ctx, true, true);
        const auto info = lc->engine->info();
        const auto cs = lc->chain_state();
        out("chain_id", info.chain_id);
        out("height", info.last_height);
        out("state_root", to_hex(info.last_state_root));
        out("genesis_hash", to_hex(lc->engine->meta().genesis_hash));
        out("last_time_ms", cs.last_time_ms);
        out("total_supply", cs.total_supply);
        out("total_minted", cs.total_minted);
        out("total_burned", cs.total_burned);
    });
}

}  // namespace yap::cli
