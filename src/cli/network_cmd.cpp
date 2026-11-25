// SPDX-License-Identifier: Apache-2.0
//
// yap network (alias: yap net) — the CLI's network mode (M2.6): talks to a CometBFT node over its RPC.
// Local dev mode (yap dev / query / tx) is untouched and never contacts a node.
//
// Trust: answers of the RPC node are not verified (no light client, no state proofs). Signatures
// protect the authorization of transactions; a node can still hide data or lie about state and
// finality. The chain id signed for is the user's expected one, never the node's.
#include <CLI11.hpp>

#include <unistd.h>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "commands.hpp"
#include "network_config.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/rpc/comet.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {
namespace {

using Json = nlohmann::ordered_json;

/// Human ("key: value" lines, like the rest of the CLI) or one stable JSON object. In JSON, every
/// u64 is a decimal string (heights, amounts in yip, nonces): no floating point anywhere.
class Report {
public:
    explicit Report(bool json) : json_(json) {}
    void str(const std::string& k, const std::string& v) {
        j_[k] = v;
        if (!json_) out(k, v);
    }
    void u64(const std::string& k, std::uint64_t v) {
        j_[k] = std::to_string(v);
        if (!json_) out(k, v);
    }
    void flag(const std::string& k, bool v) {
        j_[k] = v;
        if (!json_) out(k, v ? "yes" : "no");
    }
    void raw(const std::string& k, Json v, const std::string& human) {
        j_[k] = std::move(v);
        if (!json_) out(k, human);
    }
    void finish() const {
        if (json_) std::cout << j_.dump(2) << "\n";
    }
    [[nodiscard]] bool json() const { return json_; }

private:
    bool json_;
    Json j_ = Json::object();
};

struct NetState {
    NetworkFlags flags;
};

/// Runs a network command: maps transport/RPC failures to exit codes and, with --json, also reports
/// the error as JSON on stdout (the message always goes to stderr).
template <class F>
void guarded(const NetState& st, F&& f) {
    try {
        try {
            f();
        } catch (const rpc::TransportError& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
        } catch (const rpc::MalformedResponse& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
        } catch (const rpc::RpcError& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
        }
    } catch (const ReportedError&) {
        throw;
    } catch (const CliError& e) {
        if (st.flags.json) {
            Json j;
            j["error"] = e.what();
            j["exit_code"] = e.code();
            std::cout << j.dump(2) << "\n";
        }
        throw;
    }
}

struct Session {
    NetworkConfig cfg;
    std::unique_ptr<rpc::Client> client;
};

Session open_session(const Context& ctx, const NetState& st) {
    Session s;
    s.cfg = resolve_network_config(ctx, st.flags);
    s.client = std::make_unique<rpc::Client>(s.cfg.rpc.value, s.cfg.http());
    return s;
}

/// Latest-committed-height JSON answer of the application's Query path (ADR-0028: latest only).
std::pair<std::uint64_t, Json> app_query(rpc::Client& c, const std::string& path) {
    const auto q = c.abci_query(path);
    if (q.code != 0) {
        throw CliError(q.code == 2 ? static_cast<int>(kExitRejected) : static_cast<int>(kExitNetwork),
                       "query " + path + " failed: code " + std::to_string(q.code) + " " + q.log);
    }
    Json j = Json::parse(q.value.begin(), q.value.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw CliError(kExitNetwork, "query " + path + ": the answer is not a JSON object");
    return {q.height, std::move(j)};
}

std::string jstr(const Json& j, const char* k) {
    if (!j.contains(k) || !j[k].is_string()) throw CliError(kExitNetwork, std::string("query answer lacks '") + k + "'");
    return j[k].get<std::string>();
}

[[maybe_unused]] std::uint64_t ju64(const Json& j, const char* k) {
    const auto v = wallet::parse_decimal_u64(jstr(j, k));
    if (!v) throw CliError(kExitNetwork, std::string("query answer: '") + k + "' is not a decimal");
    return *v;
}

/// Chain identity (ADR-0029 item 7): the node's CometBFT network and the application's chain id must
/// both equal the expected chain id. `require` = an expected chain id must be configured (signing,
/// sending); otherwise read-only commands proceed with a warning.
rpc::Status check_identity(Session& s, bool require) {
    const auto status = s.client->status();
    if (!s.cfg.has_chain_id()) {
        if (require) {
            throw CliError(kExitUsage, "no expected chain id: pass --chain-id or run `yap network config set --chain-id <id>` "
                                       "(the chain id is never taken from the RPC node)");
        }
        std::cerr << "warning: chain identity not checked (no expected chain id configured); the node says '"
                  << status.chain_id << "'\n";
        return status;
    }
    if (status.chain_id != s.cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "the node at " + s.cfg.rpc.value + " is on chain '" + status.chain_id +
                                               "', expected '" + s.cfg.chain_id.value + "'");
    }
    const auto [h, chain] = app_query(*s.client, "/chain");
    if (jstr(chain, "chain_id") != s.cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "the application at " + s.cfg.rpc.value + " reports chain '" +
                                               jstr(chain, "chain_id") + "', expected '" + s.cfg.chain_id.value + "'");
    }
    return status;
}

void add_network_flags(CLI::App* app, NetState& st) {
    app->add_option("--rpc", st.flags.rpc, "CometBFT RPC endpoint, http(s)://host:port (default: config, $YAP_RPC, " +
                                               std::string(kDefaultRpc) + ")");
    app->add_option("--chain-id", st.flags.chain_id, "Expected chain id (default: config, $YAP_CHAIN_ID)");
    app->add_option("--connect-timeout-ms", st.flags.connect_timeout_ms, "TCP/TLS connect timeout");
    app->add_option("--read-timeout-ms", st.flags.read_timeout_ms, "Abort when no data arrives for this long");
    app->add_option("--timeout-ms", st.flags.timeout_ms, "Timeout of one RPC request");
    app->add_option("--max-response-bytes", st.flags.max_response_bytes, "Largest accepted RPC response");
    app->add_option("--ca-file", st.flags.ca_file, "Extra trusted CA certificate (PEM) for https endpoints");
    app->add_flag("--json", st.flags.json, "Print one JSON object (u64 values as decimal strings)");
}

void cmd_config_show(const Context& ctx, const NetState& st) {
    const auto c = resolve_network_config(ctx, st.flags);
    Report r(st.flags.json);
    r.str("config_file", network_config_path(ctx).string());
    r.str("rpc", c.rpc.value);
    r.str("rpc_source", c.rpc.source);
    r.str("chain_id", c.chain_id.value.empty() ? "(unset)" : c.chain_id.value);
    r.str("chain_id_source", c.chain_id.source);
    r.str("address_prefix", c.hrp());
    r.u64("connect_timeout_ms", static_cast<std::uint64_t>(c.connect_timeout_ms.value));
    r.u64("read_timeout_ms", static_cast<std::uint64_t>(c.read_timeout_ms.value));
    r.u64("timeout_ms", static_cast<std::uint64_t>(c.timeout_ms.value));
    r.u64("max_response_bytes", c.max_response_bytes.value);
    r.str("ca_file", c.ca_file.value.empty() ? "(system CA store)" : c.ca_file.value);
    r.finish();
}

void cmd_status(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    const auto status = check_identity(s, false);
    Report r(st.flags.json);
    r.str("rpc", s.cfg.rpc.value);
    r.str("chain_id", status.chain_id);
    r.str("expected_chain_id", s.cfg.has_chain_id() ? s.cfg.chain_id.value : "(unset)");
    r.flag("chain_id_verified", s.cfg.has_chain_id());
    r.str("node_id", status.node_id);
    r.str("moniker", status.moniker);
    r.str("version", status.version);
    r.u64("latest_height", status.latest_height);
    r.str("latest_block_hash", status.latest_block_hash);
    r.str("latest_app_hash", status.latest_app_hash);  // state root after latest_height - 1
    r.str("latest_block_time", status.latest_block_time);
    r.u64("earliest_height", status.earliest_height);
    r.flag("catching_up", status.catching_up);
    r.u64("voting_power", status.voting_power);
    r.str("trust", "unverified RPC answer (no light client)");
    r.finish();
}

void cmd_info(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto ni = s.client->net_info();
    Report r(st.flags.json);
    r.flag("listening", ni.listening);
    r.u64("peers", ni.n_peers);
    Json peers = Json::array();
    for (const auto& p : ni.peers) {
        peers.push_back({{"node_id", p.node_id}, {"moniker", p.moniker}, {"remote_ip", p.remote_ip}, {"outbound", p.outbound}});
        if (!r.json()) out("peer", p.node_id + " " + p.moniker + " " + p.remote_ip + (p.outbound ? " out" : " in"));
    }
    if (r.json()) r.raw("peer_list", peers, "");
    r.finish();
}

}  // namespace

void register_network(CLI::App& app, Context& ctx) {
    auto st = std::make_shared<NetState>();
    auto* net = app.add_subcommand("network", "Talk to a YapChain network through a CometBFT RPC endpoint "
                                              "(exit codes 6 network, 7 not found, 8 unknown outcome, "
                                              "9 execution failed, 10 chain mismatch)");
    net->alias("net");
    net->require_subcommand(1);
    add_network_flags(net, *st);

    auto* cfg = net->add_subcommand("config", "Show or set the network configuration (<home>/network.json)");
    cfg->require_subcommand(1);
    auto* show = cfg->add_subcommand("show", "Effective settings and where each comes from");
    show->callback([&ctx, st] { guarded(*st, [&] { cmd_config_show(ctx, *st); }); });
    auto* set = cfg->add_subcommand("set", "Store --rpc / --chain-id / timeouts / --ca-file in the config file");
    set->callback([&ctx, st] {
        guarded(*st, [&] {
            save_network_config(ctx, st->flags);
            NetState shown;  // show the stored file values, not the flags
            shown.flags.json = st->flags.json;
            cmd_config_show(ctx, shown);
        });
    });

    net->add_subcommand("status", "Node status: chain id, height, hashes, catching_up")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_status(ctx, *st); });
    });
    net->add_subcommand("info", "Node network information: peers")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_info(ctx, *st); });
    });

    for (auto* sub : net->get_subcommands({})) sub->fallthrough();
    cfg->fallthrough();
    for (auto* sub : cfg->get_subcommands({})) sub->fallthrough();
}

}  // namespace yap::cli
