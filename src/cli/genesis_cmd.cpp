// SPDX-License-Identifier: Apache-2.0
// yap genesis new|validate|hash
#include <CLI11.hpp>

#include <fstream>
#include <memory>
#include <sstream>

#include "commands.hpp"
#include "yap/chain/engine.hpp"
#include "yap/common/hex.hpp"
#include "yap/state/state.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {

chain::Genesis load_genesis(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw CliError(kExitUsage, "cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    auto g = chain::parse_genesis_json(ss.str());
    if (!g) throw CliError(kExitRejected, "invalid genesis " + path.string() + ": " + g.error());
    return std::move(g).value();
}

namespace {

struct NewArgs {
    std::string chain_id = "yapchain-dev-1";
    std::uint64_t time_ms = 1'767'225'600'000;  // fixed default: reproducible genesis files
    std::vector<std::string> accounts;
    std::vector<std::string> validators;
    std::string out;
};

chain::Genesis build_genesis(const NewArgs& a) {
    chain::Genesis g;
    g.chain_id = a.chain_id;
    g.genesis_time_ms = a.time_ms;
    g.params = tx::testnet_params();  // economics.md §8 (testnet only)
    for (const auto& spec : a.accounts) {
        const auto eq = spec.find('=');
        if (eq == std::string::npos) throw CliError(kExitUsage, "--account expects ADDRESS=AMOUNT, got '" + spec + "'");
        auto id = crypto::decode_address(spec.substr(0, eq), crypto::kTestnetHrp);
        if (!id) throw CliError(kExitRejected, "invalid address in --account '" + spec + "'");
        const auto amount = wallet::parse_amount(spec.substr(eq + 1));
        if (!amount) throw CliError(kExitRejected, "invalid amount in --account '" + spec + "'");
        g.accounts.push_back({*id, *amount});
    }
    std::sort(g.accounts.begin(), g.accounts.end(), [](const auto& x, const auto& y) { return x.account_id < y.account_id; });
    for (const auto& spec : a.validators) {
        // PUBKEY_HEX:POWER:REWARD_ADDRESS:NAME (the name may contain ':')
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (int i = 0; i < 3; ++i) {
            const auto c = spec.find(':', start);
            if (c == std::string::npos) throw CliError(kExitUsage, "--validator expects PUBKEY:POWER:REWARD_ADDRESS:NAME");
            parts.push_back(spec.substr(start, c - start));
            start = c + 1;
        }
        parts.push_back(spec.substr(start));
        auto pk = from_hex_fixed<32>(parts[0]);
        if (!pk) throw CliError(kExitRejected, "validator public key must be 64 lowercase hex characters");
        const auto power = wallet::parse_amount(parts[1]);  // plain integer
        auto reward = crypto::decode_address(parts[2], crypto::kTestnetHrp);
        if (!power || !reward) throw CliError(kExitRejected, "invalid --validator '" + spec + "'");
        g.validators.push_back({*pk, *power, *reward, parts[3]});
    }
    std::sort(g.validators.begin(), g.validators.end(), [](const auto& x, const auto& y) {
        return state::consensus_address(x.consensus_pubkey) < state::consensus_address(y.consensus_pubkey);
    });
    if (auto err = chain::validate_genesis(g); !err.empty()) throw CliError(kExitRejected, "genesis invalid: " + err);
    return g;
}

Hash32 initial_root(const chain::Genesis& g) {
    storage::MemoryStore store;
    chain::Engine e(store);
    return e.init_chain(g);
}

}  // namespace

void register_genesis(CLI::App& app, Context& /*ctx*/) {
    auto* gen = app.add_subcommand("genesis", "Create and check genesis files (protocol.md §11)");
    gen->require_subcommand(1);

    auto a = std::make_shared<NewArgs>();
    auto* n = gen->add_subcommand("new", "Write a genesis file with testnet parameters");
    n->add_option("--chain-id", a->chain_id, "Chain id")->capture_default_str();
    n->add_option("--time-ms", a->time_ms, "Genesis time, unix ms")->capture_default_str();
    n->add_option("--account", a->accounts, "ADDRESS=AMOUNT (repeatable)");
    n->add_option("--validator", a->validators, "PUBKEY_HEX:POWER:REWARD_ADDRESS:NAME (repeatable)")->required();
    n->add_option("--out", a->out, "Output file")->required();
    n->callback([a] {
        const auto g = build_genesis(*a);
        const auto text = chain::to_genesis_json(g);
        write_file_bytes(a->out, as_bytes(text));  // never overwrites (O_EXCL)
        out("genesis_file", a->out);
        out("genesis_hash", to_hex(chain::genesis_hash(g)));
    });

    auto file = std::make_shared<std::string>();
    auto* v = gen->add_subcommand("validate", "Strictly parse and validate a genesis file");
    v->add_option("file", *file, "Genesis JSON")->required();
    v->callback([file] {
        const auto g = load_genesis(*file);
        std::uint64_t supply = 0;
        for (const auto& acc : g.accounts) supply += acc.balance;  // validated: no overflow
        out("valid", "yes");
        out("chain_id", g.chain_id);
        out("accounts", g.accounts.size());
        out("validators", g.validators.size());
        out("issuers", g.issuers.size());
        out("total_supply", wallet::format_amount(supply));
        out("genesis_hash", to_hex(chain::genesis_hash(g)));
    });

    auto hfile = std::make_shared<std::string>();
    auto* h = gen->add_subcommand("hash", "Print the genesis hash and the initial state root");
    h->add_option("file", *hfile, "Genesis JSON")->required();
    h->callback([hfile] {
        const auto g = load_genesis(*hfile);
        out("genesis_hash", to_hex(chain::genesis_hash(g)));
        out("initial_state_root", to_hex(initial_root(g)));
    });
}

}  // namespace yap::cli
